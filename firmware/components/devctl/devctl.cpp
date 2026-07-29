// Implements the @-prefixed device-control protocol documented in
// tools/devctl.py. Traffic shares the USB-Serial/JTAG CDC link with normal
// ESP_LOG output; only lines starting with '@' are commands, everything else
// is ignored (it is either console noise or a stray log byte).
//
// Two things make this fiddly:
//  - @shot's binary payload must never interleave with a concurrent ESP_LOG
//    line, so logging is muted for the header+payload write and restored
//    right after.
//  - the LVGL screenshot must be taken and copied out while holding the LVGL
//    lock, but the lock must be released before the (slow, ~500KB/s) serial
//    transfer starts, or the UI freezes for the whole transfer.

#include "devctl.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/time.h>

#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

namespace devctl {
namespace {

const char* TAG = "devctl";

// ---------------------------------------------------------------------------
// Virtual pointer indev for @tap / @swipe
//
// A tap or swipe is expanded up front into a short list of timed samples
// (x, y, pressed, offset-from-start-in-us). The indev read callback just
// looks up "what sample should be showing right now" against a wall clock
// started when the command arrived; no separate task or queue is needed.
// Held under g_touch_mux because the RX task writes it and the LVGL task
// reads it from a different context.

struct TouchSample {
  int32_t x, y;
  bool pressed;
  int64_t at_us;  // offset from the sequence start
};

constexpr int kMaxSamples = 256;
constexpr int64_t kSwipeStepUs = 15 * 1000;

portMUX_TYPE g_touch_mux = portMUX_INITIALIZER_UNLOCKED;
TouchSample g_samples[kMaxSamples];
int g_sample_count = 0;
int64_t g_seq_start_us = 0;
int32_t g_last_x = 0, g_last_y = 0;
bool g_last_pressed = false;

void queue_tap(int32_t x, int32_t y) {
  taskENTER_CRITICAL(&g_touch_mux);
  g_samples[0] = {x, y, true, 0};
  g_samples[1] = {x, y, false, 80 * 1000};
  g_sample_count = 2;
  g_seq_start_us = esp_timer_get_time();
  taskEXIT_CRITICAL(&g_touch_mux);
}

void queue_swipe(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t ms) {
  if (ms < 0) ms = 0;
  const int64_t total_us = static_cast<int64_t>(ms) * 1000;
  // Leave room for the trailing release sample.
  int steps = static_cast<int>(total_us / kSwipeStepUs) + 1;
  if (steps > kMaxSamples - 2) steps = kMaxSamples - 2;
  if (steps < 1) steps = 1;

  taskENTER_CRITICAL(&g_touch_mux);
  int n = 0;
  for (int i = 0; i <= steps; ++i) {
    const int64_t at = (total_us * i) / steps;
    const int32_t x = x1 + static_cast<int32_t>((x2 - x1) * i / steps);
    const int32_t y = y1 + static_cast<int32_t>((y2 - y1) * i / steps);
    g_samples[n++] = {x, y, true, at};
  }
  g_samples[n++] = {x2, y2, false, total_us + 1};
  g_sample_count = n;
  g_seq_start_us = esp_timer_get_time();
  taskEXIT_CRITICAL(&g_touch_mux);
}

void indev_read_cb(lv_indev_t*, lv_indev_data_t* data) {
  taskENTER_CRITICAL(&g_touch_mux);
  if (g_sample_count > 0) {
    const int64_t elapsed = esp_timer_get_time() - g_seq_start_us;
    int idx = 0;
    while (idx + 1 < g_sample_count && g_samples[idx + 1].at_us <= elapsed) ++idx;
    g_last_x = g_samples[idx].x;
    g_last_y = g_samples[idx].y;
    g_last_pressed = g_samples[idx].pressed;
    if (idx == g_sample_count - 1) {
      // Sequence fully played out; nothing left to advance through next time.
      g_sample_count = 0;
    }
  }
  data->point.x = g_last_x;
  data->point.y = g_last_y;
  data->state = g_last_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  taskEXIT_CRITICAL(&g_touch_mux);
}

// ---------------------------------------------------------------------------
// TX helpers

int silent_vprintf(const char*, va_list) { return 0; }

void write_all(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  usb_serial_jtag_write_bytes(p, len, pdMS_TO_TICKS(200));
}

void reply_ok(const char* text) {
  char buf[96];
  const int n = std::snprintf(buf, sizeof(buf), "@ok %s\n", text);
  write_all(buf, n > 0 ? static_cast<size_t>(n) : 0);
}

void reply_err(const char* text) {
  char buf[128];
  const int n = std::snprintf(buf, sizeof(buf), "@err %s\n", text);
  write_all(buf, n > 0 ? static_cast<size_t>(n) : 0);
}

// ---------------------------------------------------------------------------
// @shot

// Sends "@shot <w> <h> rgb565 <nbytes>\n" then nbytes of tightly-packed raw
// pixels. Logging is muted for the whole thing so an interleaved ESP_LOG line
// can't corrupt the binary stream the host is counting bytes against.
void handle_shot() {
  if (!lvgl_port_lock(pdMS_TO_TICKS(2000))) {
    reply_err("could not take the LVGL lock");
    return;
  }

  lv_draw_buf_t* snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
  if (snap == nullptr) {
    lvgl_port_unlock();
    // Most likely cause: LVGL's malloc source can't satisfy a ~135KB
    // allocation. See CONFIG_LV_USE_CLIB_MALLOC in sdkconfig.defaults, which
    // routes lv_malloc() through the general heap (and so PSRAM) instead of
    // LVGL's small built-in pool.
    reply_err("lv_snapshot_take failed (out of memory?)");
    return;
  }

  const int32_t w = snap->header.w;
  const int32_t h = snap->header.h;
  const uint32_t stride = snap->header.stride;
  const size_t row_bytes = static_cast<size_t>(w) * 2;
  const size_t nbytes = row_bytes * static_cast<size_t>(h);

  uint8_t* out = static_cast<uint8_t*>(std::malloc(nbytes));
  if (out == nullptr) {
    lv_draw_buf_destroy(snap);
    lvgl_port_unlock();
    reply_err("out of memory copying snapshot");
    return;
  }

  // The draw buffer's stride can exceed w*2 (row padding/alignment), so copy
  // row by row rather than as one block.
  for (int32_t row = 0; row < h; ++row) {
    std::memcpy(out + row * row_bytes, snap->data + row * stride, row_bytes);
  }
  lv_draw_buf_destroy(snap);

  // Release the lock before the slow serial transfer: holding it would
  // freeze the UI (and the real touch indev) for however long ~135KB takes
  // to trickle out at native USB CDC's ~500KB/s.
  lvgl_port_unlock();

  vprintf_like_t prev = esp_log_set_vprintf(silent_vprintf);
  std::fflush(stdout);

  char header[64];
  const int hn = std::snprintf(header, sizeof(header), "@shot %" PRId32 " %" PRId32
                                " rgb565 %zu\n", w, h, nbytes);
  bool ok = hn > 0 && usb_serial_jtag_write_bytes(header, static_cast<size_t>(hn),
                                                   pdMS_TO_TICKS(1000)) ==
                          static_cast<int>(hn);

  // Each write must fit as a single item in the driver's TX ring buffer
  // (max item size is roughly half the buffer): an oversized write fails
  // instantly with 0 rather than blocking, which reads as a dead transfer.
  constexpr size_t kChunk = 2048;
  size_t sent = 0;
  while (ok && sent < nbytes) {
    const size_t n = std::min(kChunk, nbytes - sent);
    const int written = usb_serial_jtag_write_bytes(out + sent, n, pdMS_TO_TICKS(1000));
    if (written <= 0) {
      ok = false;
      break;
    }
    sent += static_cast<size_t>(written);
  }

  std::fflush(stdout);
  esp_log_set_vprintf(prev);

  std::free(out);

  if (!ok) {
    ESP_LOGE(TAG, "@shot transfer aborted after %zu/%zu bytes", sent, nbytes);
  }
}

// ---------------------------------------------------------------------------
// Command dispatch

void (*g_cal_cb)() = nullptr;
void (*g_time_cb)(int64_t) = nullptr;

void handle_line(char* line) {
  if (std::strcmp(line, "@ping") == 0) {
    reply_ok("pong");
    return;
  }
  if (std::strcmp(line, "@cal") == 0) {
    if (g_cal_cb == nullptr) {
      reply_err("no calibration handler registered");
    } else {
      g_cal_cb();
      reply_ok("calibration started");
    }
    return;
  }
  if (std::strcmp(line, "@shot") == 0) {
    handle_shot();
    return;
  }
  int x, y, x1, y1, x2, y2, ms;
  if (std::sscanf(line, "@tap %d %d", &x, &y) == 2) {
    queue_tap(x, y);
    reply_ok("tap queued");
    return;
  }
  if (std::sscanf(line, "@swipe %d %d %d %d %d", &x1, &y1, &x2, &y2, &ms) == 5) {
    queue_swipe(x1, y1, x2, y2, ms);
    reply_ok("swipe queued");
    return;
  }
  // There is no RTC on this board (see CLAUDE.md): the clock starts every
  // boot at whatever newlib defaults to and drifts from there. @time is how
  // the host (which does have a real clock) hands over a wall-clock reading.
  long long epoch;
  if (std::sscanf(line, "@time %lld", &epoch) == 1) {
    struct timeval tv = {};
    tv.tv_sec = static_cast<time_t>(epoch);
    tv.tv_usec = 0;
    if (settimeofday(&tv, nullptr) != 0) {
      reply_err("settimeofday failed");
      return;
    }
    if (g_time_cb != nullptr) g_time_cb(static_cast<int64_t>(epoch));
    reply_ok("time set");
    return;
  }
  reply_err("unrecognised command");
}

// ---------------------------------------------------------------------------
// RX task

void rx_task(void*) {
  constexpr size_t kLineCap = 128;
  char line[kLineCap];
  size_t len = 0;
  uint8_t chunk[64];

  while (true) {
    const int n = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(50));
    for (int i = 0; i < n; ++i) {
      const char c = static_cast<char>(chunk[i]);
      if (c == '\n' || c == '\r') {
        if (len > 0) {
          line[len] = '\0';
          if (line[0] == '@') handle_line(line);
          len = 0;
        }
        continue;
      }
      if (len < kLineCap - 1) {
        line[len++] = c;
      } else {
        // Line too long to be a real command; drop it rather than overflow.
        len = 0;
      }
    }
  }
}

}  // namespace

void init(void (*on_cal)(), void (*on_time_set)(int64_t)) {
  g_cal_cb = on_cal;
  g_time_cb = on_time_set;
  usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  // The default 256-byte TX ring buffer cannot hold a @shot chunk (writes
  // larger than an item the ring can hold fail immediately, they don't block).
  cfg.tx_buffer_size = 8192;
  const esp_err_t err = usb_serial_jtag_driver_install(&cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %s", esp_err_to_name(err));
    return;
  }
  // Route the console (ESP_LOG, stdout) through the driver we just installed
  // so logging keeps working alongside our own reads/writes on the same port.
  usb_serial_jtag_vfs_use_driver();

  if (lvgl_port_lock(pdMS_TO_TICKS(2000))) {
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_read_cb);
    lv_indev_set_display(indev, lv_display_get_default());
    lvgl_port_unlock();
  } else {
    ESP_LOGE(TAG, "could not take the LVGL lock to register the devctl indev");
  }

  xTaskCreate(rx_task, "devctl_rx", 6144, nullptr, 3, nullptr);
  ESP_LOGI(TAG, "devctl ready (@ping, @shot, @tap, @swipe, @time)");
}

}  // namespace devctl
