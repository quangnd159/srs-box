// SRS Stick — spaced-repetition flashcards on a DB MEGA3D 3ST.
//
// Brings up the panel and touch controller, loads the deck embedded in flash,
// and runs the review loop. The three physical grade/reveal buttons are
// confirmed on-device (see board_config.h and docs/pinout.md) and drive the
// same reveal/grade callbacks as touch.
//
// Scheduling state lives in RAM, rebuilt at boot by replaying the review log
// persisted on LittleFS (firmware/components/persist/) — the review log is
// the source of truth in session.h, and due dates are only ever a checkpoint
// derived from it. There is no RTC on this board, so the wall clock is
// likewise recovered from the same filesystem; see restore_clock_if_unset().

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>
#include <sys/time.h>

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_nv3023.h>
#include <esp_lcd_touch_cst816s.h>
#include <esp_littlefs.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include "board_config.h"
#include "deck.h"
#include "devctl.h"
#include "fsrs.h"
#include "persist.h"
#include "review_ui.h"
#include "session.h"

// The deck compiled by tools/deckc.py, linked into the binary.
extern const uint8_t kDeckStart[] asm("_binary_hsk1_2_srs_start");
extern const uint8_t kDeckEnd[] asm("_binary_hsk1_2_srs_end");

namespace {

const char* TAG = "srs";

// Never ESP_ERROR_CHECK a hardware call on this board. It aborts, which
// resets, which stops driving PIN_POWER_ENABLE, which lets the board switch
// itself off and vanish from USB with no log at all. Log and continue.
// This cost two debugging cycles; see docs/pinout.md.
bool ok(const char* what, esp_err_t err) {
  if (err == ESP_OK) return true;
  ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(err));
  return false;
}

esp_lcd_panel_handle_t g_panel = nullptr;
i2c_master_bus_handle_t g_i2c = nullptr;
lv_display_t* g_disp = nullptr;
// True while the @cal dot routine runs; enables RAWTOUCH logging.
volatile bool g_cal_active = false;

// LittleFS lives on the "storage" partition (see firmware/partitions.csv).
// Mounted once at boot; g_fs_ok gates every later file access so a mount or
// write failure degrades to RAM-only operation instead of crashing (same
// policy as the `ok()` helper below for hardware calls).
constexpr const char* kMountPoint = "/data";
constexpr const char* kPartitionLabel = "storage";
constexpr const char* kReviewLogPath = "/data/revlog.bin";
constexpr const char* kTimePath = "/data/lastknowntime.bin";
bool g_fs_ok = false;

deck::Deck g_deck;
session::Session* g_session = nullptr;
int g_current = -1;
bool g_revealed = false;
// Seeded at boot from the replayed review log (see persist::reviewed_today),
// so a reboot mid-session doesn't reset the count to zero; incremented live
// thereafter exactly as before.
int g_reviewed_today = 0;

// Card text arrives as length-counted slices of the deck's text blob, which
// is not null-terminated. LVGL needs C strings, so copy into fixed buffers.
char g_front[64];
char g_reading[96];
char g_back[256];
char g_ivl[4][16];

void copy_str(char* dst, size_t cap, const deck::Str& s) {
  const size_t n = s.len < cap - 1 ? s.len : cap - 1;
  if (s.data && n) std::memcpy(dst, s.data, n);
  dst[n] = '\0';
}

// "45s", "10m", "3d", "2.1mo" — the labels Anki puts on its grade buttons.
void format_interval(char* out, size_t cap, int64_t seconds) {
  if (seconds < 60) {
    std::snprintf(out, cap, "%ds", static_cast<int>(seconds));
  } else if (seconds < 3600) {
    std::snprintf(out, cap, "%dm", static_cast<int>(seconds / 60));
  } else if (seconds < 86400) {
    std::snprintf(out, cap, "%dh", static_cast<int>(seconds / 3600));
  } else if (seconds < 86400 * 30) {
    std::snprintf(out, cap, "%dd", static_cast<int>(seconds / 86400));
  } else {
    std::snprintf(out, cap, "%.1fmo", static_cast<double>(seconds) / (86400.0 * 30));
  }
}

int64_t now_seconds() {
  // Real wall-clock time, set either by @time from the host or restored at
  // boot from the last-known-time file (see restore_clock_if_unset()). Until
  // one of those happens this reads as seconds since 1970, same as any
  // freshly-booted device with no RTC.
  return static_cast<int64_t>(std::time(nullptr));
}

// ---------------------------------------------------------------------------
// Persistence: LittleFS mount, review-log replay, and the last-known-time
// fallback for a board with no RTC. See firmware/components/persist/.

bool littlefs_init() {
  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = kMountPoint;
  conf.partition_label = kPartitionLabel;
  conf.format_if_mount_failed = true;
  conf.dont_mount = false;

  const esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_vfs_littlefs_register failed: %s — running RAM-only "
                  "this boot, review history and time will not survive a reboot",
             esp_err_to_name(err));
    return false;
  }

  size_t total = 0, used = 0;
  esp_littlefs_info(kPartitionLabel, &total, &used);
  ESP_LOGI(TAG, "LittleFS mounted at %s: %zu/%zu bytes used", kMountPoint, used, total);
  return true;
}

// Persists "now" as the last-known time. Called on every grade and every
// ~60s from the heartbeat loop, so a later reboot has a recent fallback.
void persist_time_now() {
  if (!g_fs_ok) return;
  if (!persist::save_time(kTimePath, now_seconds())) {
    ESP_LOGW(TAG, "failed to persist last-known time");
  }
}

// Runs after @time sets the clock (see devctl::init below): persist the new
// value immediately rather than waiting for the next periodic tick, in case
// power is lost soon after syncing.
void on_time_synced(int64_t epoch) {
  ESP_LOGI(TAG, "clock set via @time to %" PRId64, epoch);
  persist_time_now();
}

// Runs when the host sends @gap <y>: apply a new panel y-offset live and
// force a full redraw so the whole window lands at the new position. For
// finding the exact write-window/glass alignment without reflashing; once
// settled the value belongs in LCD_OFFSET_Y.
void on_gap_adjust(int y_gap) {
  esp_lcd_panel_set_gap(g_panel, LCD_OFFSET_X, y_gap);
  if (lvgl_port_lock(pdMS_TO_TICKS(1000))) {
    lv_obj_invalidate(lv_screen_active());
    lvgl_port_unlock();
  }
  ESP_LOGI(TAG, "panel y_gap set to %d via @gap", y_gap);
}

// If the system clock still reads as unset (this board has no RTC, so every
// boot starts at the newlib default of 1970), restore whatever time was last
// persisted. This can only make cards read as due *later* than they truly
// are — time simply didn't advance while the device sat there powered off —
// never earlier, which is the safe direction: a card is never sprung on the
// user before it was actually due.
void restore_clock_if_unset() {
  const std::time_t t = std::time(nullptr);
  struct std::tm utc {};
  gmtime_r(&t, &utc);
  if (utc.tm_year + 1900 >= 2020) return;  // clock already looks like a real time

  if (!g_fs_ok) {
    ESP_LOGW(TAG, "clock unset and no filesystem to recover a last-known time from; "
                  "waiting for @time");
    return;
  }
  int64_t restored = 0;
  if (persist::load_time(kTimePath, &restored)) {
    struct timeval tv = {};
    tv.tv_sec = static_cast<time_t>(restored);
    settimeofday(&tv, nullptr);
    ESP_LOGI(TAG, "clock unset at boot; restored last-known time %" PRId64, restored);
  } else {
    ESP_LOGW(TAG, "clock unset and no persisted time found; waiting for @time");
  }
}

// ---------------------------------------------------------------------------

void power_on() {
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << PIN_POWER_ENABLE;
  cfg.mode = GPIO_MODE_OUTPUT;
  ok("gpio_config", gpio_config(&cfg));
  ok("gpio_set_level", gpio_set_level(PIN_POWER_ENABLE, 1));
  ESP_LOGI(TAG, "power enable GPIO%d high", PIN_POWER_ENABLE);
  vTaskDelay(pdMS_TO_TICKS(50));
}

void backlight_init(uint8_t percent) {
  ledc_timer_config_t timer = {};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = LEDC_TIMER_10_BIT;
  timer.timer_num = LEDC_TIMER_0;
  timer.freq_hz = 20000;
  timer.clk_cfg = LEDC_AUTO_CLK;
  ok("ledc_timer_config", ledc_timer_config(&timer));

  ledc_channel_config_t ch = {};
  ch.gpio_num = PIN_LCD_BACKLIGHT;
  ch.speed_mode = LEDC_LOW_SPEED_MODE;
  ch.channel = LEDC_CHANNEL_0;
  ch.timer_sel = LEDC_TIMER_0;
  ch.duty = (1023u * percent) / 100u;
  ok("ledc_channel_config", ledc_channel_config(&ch));
  ok("ledc_update_duty", ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
}

void i2c_init_and_scan() {
  i2c_master_bus_config_t bus = {};
  bus.i2c_port = I2C_NUM_0;
  bus.sda_io_num = PIN_I2C_SDA;
  bus.scl_io_num = PIN_I2C_SCL;
  bus.clk_source = I2C_CLK_SRC_DEFAULT;
  bus.glitch_ignore_cnt = 7;
  bus.flags.enable_internal_pullup = true;
  ok("i2c_new_master_bus", i2c_new_master_bus(&bus, &g_i2c));

  ESP_LOGI(TAG, "I2C scan on SDA=%d SCL=%d", PIN_I2C_SDA, PIN_I2C_SCL);
  for (uint8_t addr = 0x08; addr < 0x78; ++addr) {
    if (i2c_master_probe(g_i2c, addr, 50) == ESP_OK) {
      const char* what = "unknown";
      if (addr == I2C_ADDR_CST816) what = "CST816 touch";
      else if (addr == I2C_ADDR_ES8311) what = "ES8311 codec";
      else if (addr == I2C_ADDR_ES7210) what = "ES7210 mics";
      ESP_LOGI(TAG, "  found 0x%02x  %s", addr, what);
    }
  }
}

void display_init() {
  spi_bus_config_t bus = {};
  bus.sclk_io_num = PIN_LCD_SCLK;
  bus.mosi_io_num = PIN_LCD_MOSI;
  bus.miso_io_num = -1;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = UI_H_RES * 80 * sizeof(uint16_t);
  ok("spi_bus_initialize", spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_spi_config_t io_cfg = {};
  io_cfg.cs_gpio_num = PIN_LCD_CS;
  io_cfg.dc_gpio_num = PIN_LCD_DC;
  io_cfg.spi_mode = 0;
  io_cfg.pclk_hz = LCD_PIXEL_CLOCK_HZ;
  io_cfg.trans_queue_depth = 10;
  io_cfg.lcd_cmd_bits = 8;
  io_cfg.lcd_param_bits = 8;

  esp_lcd_panel_io_handle_t io = nullptr;
  if (!ok("esp_lcd_new_panel_io_spi",
          esp_lcd_new_panel_io_spi(
              static_cast<esp_lcd_spi_bus_handle_t>(static_cast<int>(LCD_SPI_HOST)),
              &io_cfg, &io))) {
    return;
  }

  esp_lcd_panel_dev_config_t panel_cfg = {};
  panel_cfg.reset_gpio_num = PIN_LCD_RST;
  // Confirmed by photo: red and blue are swapped under RGB order.
  panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
  panel_cfg.bits_per_pixel = 16;
  ok("esp_lcd_new_panel_nv3023", esp_lcd_new_panel_nv3023(io, &panel_cfg, &g_panel));
  ok("esp_lcd_panel_reset", esp_lcd_panel_reset(g_panel));
  ok("esp_lcd_panel_init", esp_lcd_panel_init(g_panel));

  // Clear ALL of GRAM, not just our window. The controller has 320 rows but
  // we only ever write 284 of them; the rest keep their power-on noise, and
  // any row of it inside the visible area shows as a static strip at the
  // panel edge. Done before disp_on so the noise is never visible, and while
  // the gap is still 0 so coordinates address raw GRAM.
  {
    constexpr int kGramRows = LCD_OFFSET_Y + LCD_V_RES;  // 320, full GRAM
    constexpr int kRowsPerChunk = 20;
    const size_t chunk_px = static_cast<size_t>(LCD_H_RES) * kRowsPerChunk;
    uint16_t* black = static_cast<uint16_t*>(heap_caps_calloc(chunk_px, 2, MALLOC_CAP_DMA));
    if (black != nullptr) {
      for (int y = 0; y < kGramRows; y += kRowsPerChunk) {
        esp_lcd_panel_draw_bitmap(g_panel, 0, y, LCD_H_RES, y + kRowsPerChunk, black);
      }
      heap_caps_free(black);
    }
  }

  ok("esp_lcd_panel_disp_on_off", esp_lcd_panel_disp_on_off(g_panel, true));

  // --- LVGL ---------------------------------------------------------------
  lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
  port_cfg.task_priority = 4;
  port_cfg.task_stack = 8192;
  port_cfg.timer_period_ms = 5;
  ok("lvgl_port_init", lvgl_port_init(&port_cfg));

  lvgl_port_display_cfg_t disp_cfg = {};
  disp_cfg.io_handle = io;
  disp_cfg.panel_handle = g_panel;
  disp_cfg.buffer_size = UI_H_RES * 40;
  disp_cfg.double_buffer = false;
  disp_cfg.hres = UI_H_RES;
  disp_cfg.vres = UI_V_RES;
  disp_cfg.color_format = LV_COLOR_FORMAT_RGB565;
  disp_cfg.flags.buff_dma = true;
  disp_cfg.flags.swap_bytes = true;  // panel expects big-endian pixels
  g_disp = lvgl_port_add_disp(&disp_cfg);

  // Apply the panel transform AFTER lvgl_port_add_disp: the port applies its
  // own rotation settings (all false by default) during setup and would
  // otherwise clear these. Setting them earlier is silently undone, which is
  // exactly why the display reverted to mirrored once the interactive
  // calibration code -- which ran after LVGL was already up -- was removed.
  ok("mirror", esp_lcd_panel_mirror(g_panel, LCD_MIRROR_X, LCD_MIRROR_Y));
  ok("set_gap", esp_lcd_panel_set_gap(g_panel, LCD_OFFSET_X, LCD_OFFSET_Y));
  ESP_LOGI(TAG, "panel %dx%d gap_y=%d, UI %dx%d landscape", LCD_H_RES, LCD_V_RES,
           LCD_OFFSET_Y, UI_H_RES, UI_V_RES);
}

void touch_init() {
  esp_lcd_panel_io_handle_t tp_io = nullptr;
  // ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG() uses out-of-order designated
  // initialisers, which C++ does not allow. Set the fields explicitly.
  esp_lcd_panel_io_i2c_config_t tp_io_cfg = {};
  tp_io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS;
  tp_io_cfg.control_phase_bytes = 1;
  tp_io_cfg.dc_bit_offset = 0;
  tp_io_cfg.lcd_cmd_bits = 8;
  tp_io_cfg.lcd_param_bits = 0;
  tp_io_cfg.scl_speed_hz = 400000;
  tp_io_cfg.flags.disable_control_phase = 1;
  if (esp_lcd_new_panel_io_i2c(g_i2c, &tp_io_cfg, &tp_io) != ESP_OK) {
    ESP_LOGW(TAG, "touch IO failed; continuing without touch");
    return;
  }

  esp_lcd_touch_config_t tp_cfg = {};
  // Generous bounds so the driver never filters a raw reading before our
  // transform sees it: the film's native x runs 0..~290.
  tp_cfg.x_max = 400;
  tp_cfg.y_max = 400;
  tp_cfg.rst_gpio_num = GPIO_NUM_NC;
  tp_cfg.int_gpio_num = GPIO_NUM_NC;
  // All transform work happens in process_coordinates below; the driver's own
  // flags stay off so the raw readings arrive untouched.
  tp_cfg.flags.swap_xy = false;
  tp_cfg.flags.mirror_x = false;
  tp_cfg.flags.mirror_y = false;
  // Measured with the on-device @cal dot routine (five known targets,
  // 2026-07-29): the film's native frame is 284x240, i.e. its x axis runs
  // along the panel's LONG side. Affine fit of the tap clusters:
  //   ui_x = 223.8 - 0.9231 * raw_y   (horizontal: raw y, inverted)
  //   ui_y = -16.8 + 0.9739 * raw_x   (vertical:   raw x, direct)
  // Residuals were 2-5px on every target. Do not replace with the driver's
  // swap/mirror flags: the scales are not exactly 1.0 and the offsets are
  // real, and the driver applies its flags against the wrong axis lengths.
  tp_cfg.process_coordinates = [](esp_lcd_touch_handle_t, uint16_t* x,
                                  uint16_t* y, uint16_t*, uint8_t* point_num,
                                  uint8_t) {
    for (uint8_t i = 0; i < *point_num; ++i) {
      const int32_t raw_x = x[i];
      const int32_t raw_y = y[i];
      if (g_cal_active) ESP_LOGW(TAG, "RAWTOUCH x=%d y=%d", raw_x, raw_y);
      int32_t ux = 224 - (raw_y * 923) / 1000;
      int32_t uy = (raw_x * 974) / 1000 - 17;
      if (ux < 0) ux = 0;
      if (ux >= UI_H_RES) ux = UI_H_RES - 1;
      if (uy < 0) uy = 0;
      if (uy >= UI_V_RES) uy = UI_V_RES - 1;
      x[i] = static_cast<uint16_t>(ux);
      y[i] = static_cast<uint16_t>(uy);
    }
  };

  esp_lcd_touch_handle_t tp = nullptr;
  if (esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &tp) != ESP_OK) {
    ESP_LOGW(TAG, "CST816 init failed; continuing without touch");
    return;
  }

  // The CST816 auto-sleeps after a few seconds of no touches and then stops
  // updating its coordinate registers, which reads as "most taps do nothing"
  // when polled without the interrupt line. Register 0xFE non-zero disables
  // auto-sleep. Written directly because the esp_lcd_touch driver doesn't.
  i2c_device_config_t dis_cfg = {};
  dis_cfg.device_address = I2C_ADDR_CST816;
  dis_cfg.scl_speed_hz = 400000;
  i2c_master_dev_handle_t dis_dev = nullptr;
  if (ok("cst816 add dev", i2c_master_bus_add_device(g_i2c, &dis_cfg, &dis_dev))) {
    const uint8_t dis_auto_sleep[2] = {0xFE, 0x01};
    ok("cst816 disable auto-sleep",
       i2c_master_transmit(dis_dev, dis_auto_sleep, sizeof(dis_auto_sleep), 100));
    i2c_master_bus_rm_device(dis_dev);
  }

  lvgl_port_touch_cfg_t touch_cfg = {};
  touch_cfg.disp = g_disp;
  touch_cfg.handle = tp;
  lv_indev_t* indev = lvgl_port_add_touch(&touch_cfg);

  ESP_LOGI(TAG, "touch ready");
}

// ---------------------------------------------------------------------------
// Review loop

void refresh_counts() {
  const auto c = g_session->counts(now_seconds(), 7 * 3600);
  ui::set_counts({c.learning, c.due, c.fresh});
}

void show_current() {
  if (g_current < 0) {
    ui::show_done(g_reviewed_today);
    return;
  }
  const auto card = g_deck.at(static_cast<size_t>(g_current));
  copy_str(g_front, sizeof(g_front), card.front);
  copy_str(g_reading, sizeof(g_reading), card.reading);
  copy_str(g_back, sizeof(g_back), card.back);
  ui::show_card({g_front, g_reading, g_back}, g_revealed);

  if (g_revealed) {
    const int64_t t = now_seconds();
    for (int i = 0; i < 4; ++i) {
      format_interval(g_ivl[i], sizeof(g_ivl[i]),
                      g_session->preview_interval(
                          g_current, static_cast<fsrs::Rating>(i + 1), t));
    }
    ui::set_intervals({g_ivl[0], g_ivl[1], g_ivl[2], g_ivl[3]});
  }
}

void advance() {
  g_current = g_session->next_card(now_seconds(), 7 * 3600);
  g_revealed = false;
  refresh_counts();
  show_current();
}

void on_reveal() {
  if (g_current < 0 || g_revealed) return;
  g_revealed = true;
  show_current();
}

void on_grade(int rating) {
  if (g_current < 0 || !g_revealed) return;
  const auto card = g_deck.at(static_cast<size_t>(g_current));
  const auto entry =
      g_session->grade(g_current, static_cast<fsrs::Rating>(rating), now_seconds());

  // Persist the entry before treating it as safely recorded. If power is cut
  // right here, the worst case is losing this one grade from the next boot's
  // replay — the RAM state grade() already applied is never trusted past a
  // reboot anyway, only the log is.
  if (g_fs_ok) {
    persist::Entry pe;
    pe.card_id = entry.card_id;
    pe.reviewed = entry.reviewed;
    pe.rating = entry.rating;
    pe.state_before = entry.state_before;
    pe.duration_ms = entry.duration_ms;
    if (!persist::append(kReviewLogPath, pe)) {
      ESP_LOGE(TAG, "failed to persist review-log entry; this grade may be lost on reboot");
    }
    persist_time_now();
  }

  g_reviewed_today++;
  ESP_LOGI(TAG, "graded id=%016" PRIx64 " rating=%d  (log now %zu entries)",
           entry.card_id, rating, g_session->log().size());
  (void)card;
  advance();
}

// ---------------------------------------------------------------------------

// Physical buttons, confirmed on-device 2026-07-29 (see board_config.h and
// docs/pinout.md).
//   GPIO2  = power  -> reveal the answer (short press only; see board_config.h)
//   GPIO40 = plus   -> grade Good
//   GPIO39 = minus  -> grade Again
//
// Grades fire on release with no double-click window: any debounce delay is
// felt on every single card. See CLAUDE.md.

// Debounce lockout after acting on a release edge. Chosen to be well past
// typical mechanical-switch bounce (usually <10ms) but short enough that it
// is never felt as input lag between separate button presses.
constexpr int64_t kDebounceUs = 30 * 1000;

void button_task(void*) {
  const gpio_num_t pins[] = {PIN_BTN_POWER, PIN_BTN_PLUS, PIN_BTN_MINUS};
  gpio_config_t cfg = {};
  cfg.mode = GPIO_MODE_INPUT;
  cfg.pull_up_en = GPIO_PULLUP_ENABLE;
  cfg.pin_bit_mask = 0;
  for (auto p : pins) cfg.pin_bit_mask |= 1ULL << p;
  gpio_config(&cfg);
  vTaskDelay(pdMS_TO_TICKS(50));

  int last[3];
  int64_t locked_until_us[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) last[i] = gpio_get_level(pins[i]);
  ESP_LOGI(TAG, "buttons: GPIO%d reveal, GPIO%d Good, GPIO%d Again",
           PIN_BTN_POWER, PIN_BTN_PLUS, PIN_BTN_MINUS);

  while (true) {
    for (int i = 0; i < 3; ++i) {
      const int level = gpio_get_level(pins[i]);
      if (level == last[i]) continue;
      last[i] = level;
      if (level == 0) continue;  // 0 -> 1 is the release edge; act on that
      const int64_t now = esp_timer_get_time();
      if (now < locked_until_us[i]) continue;  // bounce within the lockout window
      locked_until_us[i] = now + kDebounceUs;
      lvgl_port_lock(0);
      ESP_LOGW(TAG, "BUTTON GPIO%d released", pins[i]);
      // Any button reveals a hidden answer; grading before recall is
      // meaningless, so nothing is lost. This matters because the reveal
      // button doubles as the power button: a long press cuts power at the
      // hardware latch, so the review loop must be fully drivable with just
      // minus and plus.
      if (!g_revealed) {
        on_reveal();
      } else if (pins[i] != PIN_BTN_POWER) {
        on_grade(pins[i] == PIN_BTN_PLUS ? 3 : 1);
      }
      lvgl_port_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

// On-device touch calibration, triggered by @cal from the host. Shows a dot
// at five known UI positions for a few seconds each while RAWTOUCH logging
// (in process_coordinates) records what the controller reports for taps on
// it. The host fits the transform offline from the (target, raw) pairs.
// Timed advancement on purpose: it needs no working touch to move on, which
// is the whole point when touch is what's being calibrated.
void cal_task(void*) {
  struct Target {
    int x, y;
  };
  // Inset corners plus centre; far enough in that a fingertip fits.
  const Target targets[] = {{30, 30}, {210, 30}, {120, 142}, {30, 254}, {210, 254}};

  g_cal_active = true;
  lvgl_port_lock(0);
  lv_obj_t* overlay = lv_obj_create(lv_screen_active());
  lv_obj_set_size(overlay, UI_H_RES, UI_V_RES);
  lv_obj_set_pos(overlay, 0, 0);
  lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(overlay, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(overlay, 0, LV_PART_MAIN);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* dot = lv_obj_create(overlay);
  lv_obj_set_size(dot, 22, 22);
  lv_obj_set_style_radius(dot, 11, LV_PART_MAIN);
  lv_obj_set_style_bg_color(dot, lv_color_hex(0xEF4444), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);

  lv_obj_t* lbl = lv_label_create(overlay);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xF5F5F5), LV_PART_MAIN);
  lv_label_set_text(lbl, "tap the dot");
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -40);
  lvgl_port_unlock();

  constexpr int kN = sizeof(targets) / sizeof(targets[0]);
  for (int k = 0; k < kN; ++k) {
    lvgl_port_lock(0);
    lv_obj_set_pos(dot, targets[k].x - 11, targets[k].y - 11);
    lvgl_port_unlock();
    ESP_LOGW(TAG, "CAL target %d shown at ui=(%d,%d)", k, targets[k].x,
             targets[k].y);
    vTaskDelay(pdMS_TO_TICKS(6000));
  }

  lvgl_port_lock(0);
  lv_obj_del(overlay);
  lvgl_port_unlock();
  g_cal_active = false;
  ESP_LOGW(TAG, "CAL done");
  vTaskDelete(nullptr);
}

void start_calibration() {
  xTaskCreate(cal_task, "cal", 4096, nullptr, 3, nullptr);
}

}  // namespace

extern "C" void app_main() {
  ESP_LOGI(TAG, "=== SRS Stick ===");

  power_on();

  // --- persistence ----------------------------------------------------------
  // Mounted before anything else touches the clock or the review log: both
  // now_seconds() and the deck/session setup below depend on it being ready
  // (or, if the mount failed, on g_fs_ok correctly gating every file access).
  g_fs_ok = littlefs_init();
  restore_clock_if_unset();

  i2c_init_and_scan();
  display_init();
  touch_init();
  backlight_init(85);

  // --- deck ---------------------------------------------------------------
  const size_t deck_size = static_cast<size_t>(kDeckEnd - kDeckStart);
  const auto err = g_deck.open(kDeckStart, deck_size, /*verify_crc=*/true);
  if (err != deck::Error::None) {
    ESP_LOGE(TAG, "deck failed to load: %s", deck::error_string(err));
    return;
  }
  ESP_LOGI(TAG, "deck loaded: %zu cards, %zu bytes", g_deck.count(), deck_size);

  static fsrs::Parameters params;
  static session::Limits limits;
  static session::Session sess(g_deck, params, limits);
  g_session = &sess;

  // --- review log replay ---------------------------------------------------
  // The review log is the source of truth (see session.h): CardState is
  // rebuilt here from scratch, before the first card is ever shown, rather
  // than trusting any state left over from before the reboot.
  if (g_fs_ok) {
    const auto loaded = persist::load(kReviewLogPath);
    if (!loaded.header_ok) {
      ESP_LOGE(TAG, "review log at %s has a bad header; starting with empty "
                    "history this boot (the file itself is left untouched)",
               kReviewLogPath);
    } else {
      if (loaded.truncated_tail) {
        ESP_LOGW(TAG, "review log had a partial trailing record (likely a power "
                      "cut mid-write); dropping it, keeping %zu earlier entries",
                 loaded.entries.size());
        if (!persist::truncate_to_complete_records(kReviewLogPath)) {
          ESP_LOGW(TAG, "could not truncate the partial tail; future appends may misalign");
        }
      }
      std::vector<session::ReviewEntry> replay_entries;
      replay_entries.reserve(loaded.entries.size());
      for (const auto& e : loaded.entries) {
        session::ReviewEntry re;
        re.card_id = e.card_id;
        re.reviewed = e.reviewed;
        re.rating = e.rating;
        re.state_before = e.state_before;
        re.duration_ms = e.duration_ms;
        replay_entries.push_back(re);
      }
      sess.replay(replay_entries.data(), replay_entries.size());
      g_reviewed_today = persist::reviewed_today(loaded.entries, now_seconds(), 7 * 3600);
      ESP_LOGI(TAG, "replayed %zu review-log entries from flash; %d reviewed today",
               replay_entries.size(), g_reviewed_today);
    }
  }

  // --- UI -----------------------------------------------------------------
  lvgl_port_lock(0);
  ui::init(UI_H_RES, UI_V_RES);

  ui::set_deck_name("HSK 1-2");
  ui::set_callbacks(on_reveal, on_grade);
  advance();
  lvgl_port_unlock();

  devctl::init(start_calibration, on_time_synced, on_gap_adjust);

  xTaskCreate(button_task, "buttons", 4096, nullptr, 3, nullptr);

  ESP_LOGI(TAG, "ready — tap the screen to reveal, then grade");

  int beat = 0;
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    ++beat;
    // Every ~60s: keeps the last-known-time file fresh even during a long
    // idle stretch with no grading, so an unclean shutdown still has a
    // recent fallback for restore_clock_if_unset() on the next boot.
    if (beat % 12 == 0) persist_time_now();
    ESP_LOGI(TAG, "alive %d", beat);
  }
}
