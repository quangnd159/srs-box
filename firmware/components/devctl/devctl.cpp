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
#include <cctype>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <driver/gpio.h>
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_err.h>
#include <esp_littlefs.h>
#include <esp_log.h>
#include <esp_rom_crc.h>
#include <esp_system.h>
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

// Like reply_ok, but with room for a full device path in the message
// ("fput decks/some-long-name.srs 4194304" can run past reply_ok's 96 bytes).
void reply_okf(const char* fmt, ...) {
  char buf[256];
  buf[0] = '@'; buf[1] = 'o'; buf[2] = 'k'; buf[3] = ' ';
  va_list ap;
  va_start(ap, fmt);
  const int n = std::vsnprintf(buf + 4, sizeof(buf) - 5, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  size_t len = 4 + static_cast<size_t>(n);
  if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
  buf[len++] = '\n';
  write_all(buf, len);
}

// ---------------------------------------------------------------------------
// File transfer (@fput / @fget / @fls / @fdel), see docs/sync-protocol.md.
//
// Paths on the wire are relative to the LittleFS mount point ("/data") and
// restricted to decks/, fonts/, and revlog.bin, matching the fixed layout
// the firmware (and the deck loader) expect. Nothing here allows writing or
// reading arbitrary files on the flash.

constexpr size_t kMaxFileBytes = 4 * 1024 * 1024;
constexpr const char* kMountPoint = "/data";
// Must match the partition label main.cpp mounts LittleFS under.
constexpr const char* kPartitionLabel = "storage";
constexpr const char* kStagePath = "/data/.stage.tmp";

// Charset and "no .." are the wire-format rules from docs/sync-protocol.md;
// applied before any path is ever handed to fopen/stat/etc.
bool path_chars_ok(const char* p) {
  if (*p == '\0') return false;
  for (const char* c = p; *c; ++c) {
    const unsigned char u = static_cast<unsigned char>(*c);
    if (!(std::isalnum(u) || u == '.' || u == '_' || u == '/' || u == '-')) return false;
  }
  return std::strstr(p, "..") == nullptr;
}

// Addressable by @fput/@fget: one of the three paths the sync protocol and
// the firmware's own deck/font/revlog loading agree on.
bool path_addressable(const char* p) {
  if (std::strcmp(p, "revlog.bin") == 0) return true;
  if (std::strncmp(p, "decks/", 6) == 0 && p[6] != '\0') return true;
  if (std::strncmp(p, "fonts/", 6) == 0 && p[6] != '\0') return true;
  return false;
}

// @fdel is narrower than @fput/@fget: revlog.bin is device-owned history and
// is never deleted over this link (see docs/sync-protocol.md).
bool path_deletable(const char* p) {
  if (std::strncmp(p, "decks/", 6) == 0 && p[6] != '\0') return true;
  if (std::strncmp(p, "fonts/", 6) == 0 && p[6] != '\0') return true;
  return false;
}

// Builds "/data/<relpath>" into `out`, returning false if it wouldn't fit.
bool full_path(const char* relpath, char* out, size_t cap) {
  const int n = std::snprintf(out, cap, "%s/%s", kMountPoint, relpath);
  return n > 0 && static_cast<size_t>(n) < cap;
}

// mkdir()s every directory component of `full`, ignoring "already exists".
// Only ever needs to create one level deep (decks/, fonts/) in practice, but
// walking the whole path costs nothing and matches what the spec promises.
void mkdir_parents(const char* full) {
  char buf[192];
  std::strncpy(buf, full, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  for (char* p = buf + 1; *p; ++p) {
    if (*p == '/') {
      *p = '\0';
      mkdir(buf, 0755);  // ignore errors; EEXIST is the expected outcome
      *p = '/';
    }
  }
}

// zlib-compatible CRC-32 (IEEE 802.3). esp_rom_crc32_le does the standard
// pre/post inversion INTERNALLY: seed it with 0 and chain its return value
// through cumulative calls, and the result equals zlib.crc32() directly.
// (Seeding 0xFFFFFFFF and inverting the result — the classic shape for a
// raw LFSR — double-inverts here and fails every transfer. Verified against
// zlib.crc32 on hardware, 2026-07-30.)
uint32_t crc32_init() { return 0; }
uint32_t crc32_update(uint32_t crc, const void* buf, size_t len) {
  return esp_rom_crc32_le(crc, static_cast<const uint8_t*>(buf), static_cast<uint32_t>(len));
}
uint32_t crc32_final(uint32_t crc) { return crc; }

// Reads exactly `nbytes` from the CDC link into `path` (already validated
// and known addressable), staging through kStagePath so a failed or aborted
// transfer never disturbs an existing file. Blocks the devctl RX task for
// the duration, same as @shot blocks it on the way out; there is only one
// client on this link at a time, so that is fine.
void handle_fput(const char* path, uint32_t nbytes, const char* crc_hex) {
  if (!path_chars_ok(path) || !path_addressable(path)) {
    reply_err("bad or unaddressable path");
    return;
  }
  if (nbytes > kMaxFileBytes) {
    reply_err("file too large (max 4MB)");
    return;
  }
  uint32_t want_crc = 0;
  if (std::sscanf(crc_hex, "%" SCNx32, &want_crc) != 1) {
    reply_err("bad crc32");
    return;
  }

  size_t total = 0, used = 0;
  if (esp_littlefs_info(kPartitionLabel, &total, &used) == ESP_OK) {
    const size_t free_bytes = total > used ? total - used : 0;
    if (nbytes > free_bytes) {
      reply_err("not enough free space");
      return;
    }
  }

  char target[192];
  if (!full_path(path, target, sizeof(target))) {
    reply_err("path too long");
    return;
  }

  // The whole payload is buffered in RAM (PSRAM; 4MB cap vs 8MB present)
  // and written to flash only after the stream ends. This is load-bearing,
  // not an optimisation: writing to LittleFS *during* the transfer stalls
  // the drain whenever the filesystem pauses to erase blocks, the USB pipe
  // NAKs for the duration, and past a few seconds of NAK macOS silently
  // drops the in-flight tail of the stream — observed as a transfer that
  // reliably dies ~20KB short once the partition had churned through a few
  // font-sized rewrites. A drain that never blocks on flash can't trigger it.
  uint8_t* body = static_cast<uint8_t*>(std::malloc(nbytes));
  if (body == nullptr) {
    reply_err("out of memory buffering upload");
    return;
  }

  reply_ok("send");

  // No log-muting here (unlike @fget/@shot): those mute because ESP_LOG
  // shares the same *outbound* pipe as the binary payload the device is
  // sending. Here the payload is inbound; nothing the device writes to the
  // console can corrupt what the host is streaming to it.
  size_t received = 0;
  int64_t last_activity = esp_timer_get_time();
  while (received < nbytes) {
    const int n = usb_serial_jtag_read_bytes(body + received, nbytes - received,
                                             pdMS_TO_TICKS(200));
    if (n > 0) {
      received += static_cast<size_t>(n);
      last_activity = esp_timer_get_time();
    } else if (esp_timer_get_time() - last_activity > 10 * 1000 * 1000) {
      // Report how far the stream got: "0/N" means the payload never reached
      // the driver at all, a partial count means the drain stalled mid-file.
      char msg[64];
      std::snprintf(msg, sizeof(msg), "timeout after %zu/%" PRIu32 " bytes",
                    received, nbytes);
      reply_err(msg);
      std::free(body);
      return;
    }
  }

  const uint32_t final_crc = crc32_final(crc32_update(crc32_init(), body, nbytes));
  if (final_crc != want_crc) {
    reply_err("crc mismatch");
    std::free(body);
    return;
  }

  FILE* f = std::fopen(kStagePath, "wb");
  if (f == nullptr) {
    reply_err("could not open staging file");
    std::free(body);
    return;
  }
  const bool wrote = std::fwrite(body, 1, nbytes, f) == nbytes;
  std::free(body);
  std::fclose(f);
  if (!wrote) {
    reply_err("write failed");
    std::remove(kStagePath);
    return;
  }

  mkdir_parents(target);
  if (std::rename(kStagePath, target) != 0) {
    reply_err("rename failed");
    std::remove(kStagePath);
    return;
  }

  reply_okf("fput %s %" PRIu32, path, nbytes);
}

// Streams a file back to the host: "@fget <nbytes> <crc32>\n" then the raw
// bytes, logging muted for the duration exactly as @shot does (the payload
// shares the outbound pipe with ESP_LOG here).
void handle_fget(const char* path) {
  if (!path_chars_ok(path) || !path_addressable(path)) {
    reply_err("bad or unaddressable path");
    return;
  }
  char full[192];
  if (!full_path(path, full, sizeof(full))) {
    reply_err("path too long");
    return;
  }
  FILE* f = std::fopen(full, "rb");
  if (f == nullptr) {
    reply_err("not found");
    return;
  }

  // First pass: size + crc32 only, no serial traffic yet.
  constexpr size_t kChunk = 2048;
  uint8_t buf[kChunk];
  uint32_t crc = crc32_init();
  size_t nbytes = 0;
  size_t n;
  while ((n = std::fread(buf, 1, kChunk, f)) > 0) {
    crc = crc32_update(crc, buf, n);
    nbytes += n;
  }
  crc = crc32_final(crc);
  std::rewind(f);

  vprintf_like_t prev = esp_log_set_vprintf(silent_vprintf);
  std::fflush(stdout);

  char header[64];
  const int hn = std::snprintf(header, sizeof(header), "@fget %zu %08" PRIx32 "\n", nbytes, crc);
  bool ok = hn > 0 && usb_serial_jtag_write_bytes(header, static_cast<size_t>(hn),
                                                   pdMS_TO_TICKS(1000)) == hn;

  // Same TX-ring-item-size constraint as @shot: writes must fit a single
  // ring item, so chunk rather than handing the whole file to one write.
  size_t sent = 0;
  while (ok && sent < nbytes) {
    n = std::fread(buf, 1, kChunk, f);
    if (n == 0) {
      ok = false;
      break;
    }
    const int written = usb_serial_jtag_write_bytes(buf, n, pdMS_TO_TICKS(1000));
    if (written <= 0 || static_cast<size_t>(written) != n) {
      ok = false;
      break;
    }
    sent += n;
  }

  std::fflush(stdout);
  esp_log_set_vprintf(prev);
  std::fclose(f);

  if (!ok) {
    ESP_LOGE(TAG, "@fget transfer aborted after %zu/%zu bytes", sent, nbytes);
  }
}

// Appends " <relpath>=<size>" to the (bounded) @fls reply buffer, tracking
// whether the listing was truncated rather than overflowing it.
void fls_append(char* buf, size_t cap, size_t* len, const char* relpath, off_t size,
                 bool* truncated) {
  if (*truncated) return;
  char entry[160];
  const int n = std::snprintf(entry, sizeof(entry), " %s=%lld", relpath,
                               static_cast<long long>(size));
  if (n < 0) return;
  if (*len + static_cast<size_t>(n) >= cap) {
    *truncated = true;
    return;
  }
  std::memcpy(buf + *len, entry, static_cast<size_t>(n));
  *len += static_cast<size_t>(n);
}

// Lists one directory (dirname must end in '/', e.g. "decks/") into the
// @fls reply buffer.
void fls_list_dir(const char* dirname, char* buf, size_t cap, size_t* len, bool* truncated) {
  char dirfull[64];
  std::snprintf(dirfull, sizeof(dirfull), "%s/%s", kMountPoint, dirname);
  DIR* d = opendir(dirfull);
  if (d == nullptr) return;
  struct dirent* de;
  while (!*truncated && (de = readdir(d)) != nullptr) {
    if (de->d_name[0] == '.') continue;  // skip "." / ".." / the fput staging file
    // Sized for the gcc format-truncation checker's worst case (d_name up to
    // NAME_MAX, conservatively 255) rather than any real LittleFS filename.
    char relpath[300];
    std::snprintf(relpath, sizeof(relpath), "%s%s", dirname, de->d_name);
    char entryfull[364];
    std::snprintf(entryfull, sizeof(entryfull), "%s%s", dirfull, de->d_name);
    struct stat st;
    if (stat(entryfull, &st) == 0) {
      fls_append(buf, cap, len, relpath, st.st_size, truncated);
    }
  }
  closedir(d);
}

// Single-line reply covering decks/, fonts/, and revlog.bin. Reply buffer is
// 512 bytes per docs/sync-protocol.md; a listing too big to fit is
// truncated at a whole entry and ends with "...".
void handle_fls() {
  constexpr size_t kCap = 512;
  char buf[kCap];
  size_t len = 0;
  bool truncated = false;

  struct stat st;
  char revlog_full[64];
  std::snprintf(revlog_full, sizeof(revlog_full), "%s/revlog.bin", kMountPoint);
  if (stat(revlog_full, &st) == 0) {
    fls_append(buf, kCap, &len, "revlog.bin", st.st_size, &truncated);
  }
  fls_list_dir("decks/", buf, kCap, &len, &truncated);
  fls_list_dir("fonts/", buf, kCap, &len, &truncated);

  char out[kCap + 32];
  const int n = std::snprintf(out, sizeof(out), "@ok fls%.*s%s\n", static_cast<int>(len), buf,
                               truncated ? " ..." : "");
  write_all(out, n > 0 ? static_cast<size_t>(n) : 0);
}

void handle_fdel(const char* path) {
  if (!path_chars_ok(path) || !path_deletable(path)) {
    reply_err("bad or undeletable path");
    return;
  }
  char full[192];
  if (!full_path(path, full, sizeof(full))) {
    reply_err("path too long");
    return;
  }
  if (std::remove(full) != 0) {
    reply_err("delete failed (not found?)");
    return;
  }
  reply_okf("fdel %s", path);
}

void handle_reboot() {
  reply_ok("rebooting");
  std::fflush(stdout);
  vTaskDelay(pdMS_TO_TICKS(200));
  esp_restart();
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
// @adc / @gpin / @gpinhist -- battery ADC channel and charge-detect GPIO,
// both now confirmed (see docs/pinout.md), plus the general free-GPIO sweep
// @gpin still supports for anything else that turns up unexplained.
//
// The vendor never documented either circuit; these started as pure
// diagnostics -- point a multimeter/known battery state at the board, run
// @adc and @gpin a few times, and look for the reading that tracks it;
// @gpinhist extended @gpin across an unplug/replug window so the diff could
// be read back after the fact instead of watched live, which is how GPIO47
// (charge status) got confirmed. Nothing here is wired into review/session
// logic; all three commands remain self-contained and safe to call
// repeatedly for any future hunting.
//
// Once firmware/components/power/power.cpp has run (i.e. after the first
// power::battery_percent() call), @adc's GPIO17/ADC2 read will report -1:
// power.cpp holds the ADC_UNIT_2 handle open for the life of the process
// (see its "Lazy one-time init" comment), and a second adc_oneshot_new_unit()
// on the same unit while one is already held fails outright. That is
// expected once the real driver has claimed the unit, not a hardware fault
// -- @adc's GPIO1/GPIO3 readings on ADC1 are unaffected either way.

// GPIO1 (ADC1_CH0) and GPIO3 (ADC1_CH2) are the only two ADC1-capable pins
// this board leaves free -- every other ADC1 pin (GPIO2, GPIO4-10) is
// already claimed by a button or the display/I2C/I2S wiring (see
// board_config.h and docs/pinout.md's pin map). GPIO17 (ADC2_CH6) is sampled
// too: the stock firmware's PowerManager reads ADC_CHANNEL_6, and since
// ADC1's copy of that channel index (GPIO7) is already the ES7210 mic data
// line, the ADC2 instance is the live candidate. 12dB attenuation is used so
// the full 0-3.3V range is visible; a real battery reading behind a divider
// is expected around raw ~2300 at this attenuation, per docs/pinout.md.
void handle_adc() {
  adc_oneshot_unit_handle_t unit = nullptr;
  adc_oneshot_unit_init_cfg_t unit_cfg = {};
  unit_cfg.unit_id = ADC_UNIT_1;
  if (adc_oneshot_new_unit(&unit_cfg, &unit) != ESP_OK) {
    reply_err("adc_oneshot_new_unit failed");
    return;
  }

  const adc_channel_t channels[2] = {ADC_CHANNEL_0, ADC_CHANNEL_2};  // GPIO1, GPIO3
  adc_oneshot_chan_cfg_t chan_cfg = {};
  chan_cfg.atten = ADC_ATTEN_DB_12;
  chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  bool chan_ok[2];
  for (int i = 0; i < 2; ++i) {
    chan_ok[i] = adc_oneshot_config_channel(unit, channels[i], &chan_cfg) == ESP_OK;
    if (!chan_ok[i]) {
      ESP_LOGW(TAG, "adc_oneshot_config_channel(%d) failed", static_cast<int>(channels[i]));
    }
  }

  // Calibration is best-effort: a raw-only reading already answers "does this
  // pin track the battery or just drift like a floating pin", which is the
  // whole point of this command. Fall back to raw-only if the eFuse bits this
  // scheme needs were never burnt.
  adc_cali_handle_t cali = nullptr;
  bool have_cali = false;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  adc_cali_curve_fitting_config_t cali_cfg = {};
  cali_cfg.unit_id = ADC_UNIT_1;
  cali_cfg.atten = ADC_ATTEN_DB_12;
  cali_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
  have_cali = adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali) == ESP_OK;
#endif

  constexpr int kSamples = 32;
  int64_t sums[2] = {0, 0};
  for (int s = 0; s < kSamples; ++s) {
    for (int i = 0; i < 2; ++i) {
      if (!chan_ok[i]) continue;
      int raw = 0;
      if (adc_oneshot_read(unit, channels[i], &raw) == ESP_OK) sums[i] += raw;
    }
  }
  const int avg1 = chan_ok[0] ? static_cast<int>(sums[0] / kSamples) : -1;
  const int avg3 = chan_ok[1] ? static_cast<int>(sums[1] / kSamples) : -1;

  int mv1 = -1, mv3 = -1;
  if (have_cali) {
    if (chan_ok[0]) adc_cali_raw_to_voltage(cali, avg1, &mv1);
    if (chan_ok[1]) adc_cali_raw_to_voltage(cali, avg3, &mv3);
    adc_cali_delete_scheme_curve_fitting(cali);
  }

  // Deinit so the unit is free again for the next @adc call -- a second
  // adc_oneshot_new_unit() on the same unit while one is still held fails.
  adc_oneshot_del_unit(unit);

  // GPIO17 / ADC2_CHANNEL_6: strings pulled from the stock firmware dump show
  // its PowerManager reading ADC_CHANNEL_6 for the battery, and ADC1_CH6
  // (GPIO7) is already claimed by the ES7210 mic data line, which points at
  // the ADC2 instance of that same channel index instead. ADC2 needs its own
  // unit handle and shares its hardware with the WiFi driver, so a read can
  // legitimately fail with ESP_ERR_TIMEOUT while WiFi is active -- treated as
  // a dropped sample, not a reason to fail the whole command.
  int avg17 = -1;
  adc_oneshot_unit_handle_t unit2 = nullptr;
  adc_oneshot_unit_init_cfg_t unit2_cfg = {};
  unit2_cfg.unit_id = ADC_UNIT_2;
  if (adc_oneshot_new_unit(&unit2_cfg, &unit2) == ESP_OK) {
    adc_oneshot_chan_cfg_t chan17_cfg = {};
    chan17_cfg.atten = ADC_ATTEN_DB_12;
    chan17_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_oneshot_config_channel(unit2, ADC_CHANNEL_6, &chan17_cfg) == ESP_OK) {
      int64_t sum17 = 0;
      int n17 = 0;
      for (int s = 0; s < kSamples; ++s) {
        int raw = 0;
        if (adc_oneshot_read(unit2, ADC_CHANNEL_6, &raw) == ESP_OK) {
          sum17 += raw;
          ++n17;
        }
      }
      if (n17 > 0) avg17 = static_cast<int>(sum17 / n17);
    } else {
      ESP_LOGW(TAG, "adc_oneshot_config_channel(ADC2_CH6/GPIO17) failed");
    }
    adc_oneshot_del_unit(unit2);
  } else {
    ESP_LOGW(TAG, "adc_oneshot_new_unit(ADC_UNIT_2) failed");
  }

  if (have_cali) {
    reply_okf("adc gpio1=%d gpio1_mv=%d gpio3=%d gpio3_mv=%d gpio17=%d", avg1, mv1, avg3, mv3,
              avg17);
  } else {
    reply_okf("adc gpio1=%d gpio3=%d gpio17=%d", avg1, avg3, avg17);
  }
}

// Digital snapshot of every free, input-capable GPIO, for charge-detect
// hunting. Pins already claimed by board_config.h (the display, I2C, I2S,
// power-enable, and the three physical buttons) are excluded by construction
// -- this list only touches GPIOs nothing else in the firmware configures.
//
// GPIO45/46 are strapping pins (VDD_SPI voltage select / boot mode): read as
// plain floating input with no pulls touched, so whatever the board latched
// at reset is left undisturbed. GPIO46 is input-only on the S3 anyway, so
// this is the only mode it supports.
// GPIO41/42 double as the native USB-JTAG MTDI/MTMS lines. Reading them as
// GPIO is harmless with no debugger attached -- this command rides the CDC
// side of the same USB port -- but avoid calling @gpin while openocd is
// attached, to not disturb an active JTAG session.
// Shared with @gpinhist below, so both commands agree on which pins and in
// which bit order.
constexpr gpio_num_t kGpinPins[] = {
    GPIO_NUM_1,  GPIO_NUM_3,  GPIO_NUM_17, GPIO_NUM_21, GPIO_NUM_38,
    GPIO_NUM_41, GPIO_NUM_42, GPIO_NUM_45, GPIO_NUM_46, GPIO_NUM_47,
    GPIO_NUM_48,
};
constexpr int kGpinPinCount = sizeof(kGpinPins) / sizeof(kGpinPins[0]);
static_assert(kGpinPinCount <= 16, "gpinhist packs levels into a uint16_t bitmask");

void configure_gpin_pins() {
  gpio_config_t cfg = {};
  cfg.mode = GPIO_MODE_INPUT;
  cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.pin_bit_mask = 0;
  for (auto p : kGpinPins) cfg.pin_bit_mask |= 1ULL << p;
  gpio_config(&cfg);
}

void handle_gpin() {
  configure_gpin_pins();

  char body[192];
  int len = std::snprintf(body, sizeof(body), "gpin");
  for (int i = 0; i < kGpinPinCount && len > 0 && static_cast<size_t>(len) < sizeof(body); ++i) {
    len += std::snprintf(body + len, sizeof(body) - static_cast<size_t>(len), " %d=%d",
                          static_cast<int>(kGpinPins[i]), gpio_get_level(kGpinPins[i]));
  }
  reply_okf("%s", body);
}

// ---------------------------------------------------------------------------
// @gpinhist -- background sampler for the charge-detect diff experiment.
//
// The idea: `@gpinhist start`, then the user physically unplugs USB for
// ~15s and replugs it, then `@gpinhist` dumps the ring so the host can diff
// levels across the unplugged window and see which candidate pin (GPIO47 is
// the current suspect, see docs/pinout.md) actually tracks plug state rather
// than just reading high always. Same pin list and bit order as @gpin.
//
// Self-contained: an esp_timer periodic callback samples every 500ms into a
// fixed RAM ring of the last ~120 samples (60s), each entry a tick offset
// (ms since start) plus a packed level bitmask. No task, no dynamic memory.

constexpr int kGpinHistCapacity = 120;  // 60s at 500ms
constexpr int64_t kGpinHistPeriodUs = 500 * 1000;

esp_timer_handle_t g_gpinhist_timer = nullptr;
uint32_t g_gpinhist_tick_ms[kGpinHistCapacity];
uint16_t g_gpinhist_levels[kGpinHistCapacity];
int g_gpinhist_count = 0;   // number of valid entries (<= capacity)
int g_gpinhist_next = 0;    // next slot to write; wraps, oldest-overwrite
int64_t g_gpinhist_start_us = 0;

void gpinhist_sample_cb(void*) {
  uint16_t bits = 0;
  for (int i = 0; i < kGpinPinCount; ++i) {
    if (gpio_get_level(kGpinPins[i])) bits |= static_cast<uint16_t>(1u << i);
  }
  const uint32_t tick_ms =
      static_cast<uint32_t>((esp_timer_get_time() - g_gpinhist_start_us) / 1000);

  g_gpinhist_tick_ms[g_gpinhist_next] = tick_ms;
  g_gpinhist_levels[g_gpinhist_next] = bits;
  g_gpinhist_next = (g_gpinhist_next + 1) % kGpinHistCapacity;
  if (g_gpinhist_count < kGpinHistCapacity) ++g_gpinhist_count;
}

void gpinhist_start() {
  if (g_gpinhist_timer != nullptr) {
    esp_timer_stop(g_gpinhist_timer);
    esp_timer_delete(g_gpinhist_timer);
    g_gpinhist_timer = nullptr;
  }
  configure_gpin_pins();
  g_gpinhist_count = 0;
  g_gpinhist_next = 0;
  g_gpinhist_start_us = esp_timer_get_time();

  esp_timer_create_args_t args = {};
  args.callback = gpinhist_sample_cb;
  args.name = "gpinhist";
  if (esp_timer_create(&args, &g_gpinhist_timer) != ESP_OK) {
    reply_err("esp_timer_create failed");
    g_gpinhist_timer = nullptr;
    return;
  }
  if (esp_timer_start_periodic(g_gpinhist_timer, kGpinHistPeriodUs) != ESP_OK) {
    reply_err("esp_timer_start_periodic failed");
    esp_timer_delete(g_gpinhist_timer);
    g_gpinhist_timer = nullptr;
    return;
  }
  reply_ok("gpinhist started");
}

void gpinhist_stop() {
  if (g_gpinhist_timer != nullptr) {
    esp_timer_stop(g_gpinhist_timer);
    esp_timer_delete(g_gpinhist_timer);
    g_gpinhist_timer = nullptr;
  }
  reply_ok("gpinhist stopped");
}

// Dumps the ring oldest-first as one line per sample ("@gpinhist <i> <tick_ms>
// <bits_hex>"), then a terminating "@ok gpinhist <n>" the host can use to
// know the dump is complete. Multi-line rather than one packed line: 120
// samples would run well past the ~256-byte reply buffers used elsewhere.
void gpinhist_dump() {
  const int n = g_gpinhist_count;
  // Oldest entry is at g_gpinhist_next when the ring is full; when it isn't
  // full yet, the oldest entry is simply index 0 (nothing has wrapped).
  const int oldest = (n == kGpinHistCapacity) ? g_gpinhist_next : 0;
  for (int i = 0; i < n; ++i) {
    const int idx = (oldest + i) % kGpinHistCapacity;
    char line[48];
    const int len = std::snprintf(line, sizeof(line), "@gpinhist %d %" PRIu32 " %04x\n", i,
                                   g_gpinhist_tick_ms[idx], g_gpinhist_levels[idx]);
    if (len > 0) write_all(line, static_cast<size_t>(len));
  }
  reply_okf("gpinhist %d", n);
}

void handle_gpinhist(const char* arg) {
  if (std::strcmp(arg, "start") == 0) {
    gpinhist_start();
  } else if (std::strcmp(arg, "stop") == 0) {
    gpinhist_stop();
  } else if (arg[0] == '\0') {
    gpinhist_dump();
  } else {
    reply_err("usage: @gpinhist [start|stop]");
  }
}

// ---------------------------------------------------------------------------
// Command dispatch

void (*g_cal_cb)() = nullptr;
void (*g_time_cb)(int64_t) = nullptr;
void (*g_gap_cb)(int) = nullptr;
void (*g_stat_cb)(char*, size_t) = nullptr;

// @stat: device-state snapshot for the sync host. The JSON body is built by
// main.cpp's callback (registered via devctl::init) since it is the only
// place that knows about deck slots, the review log, and battery status;
// devctl only wraps it as "@ok stat <json>". A dedicated buffer rather than
// reply_okf's 256 bytes: a deck list can run well past that once there are a
// handful of decks, and the instructions explicitly allow a long line here.
void handle_stat() {
  if (g_stat_cb == nullptr) {
    reply_err("no stat handler registered");
    return;
  }
  static char json[2048];
  json[0] = '\0';
  g_stat_cb(json, sizeof(json));

  static char out[2048 + 16];
  const int n = std::snprintf(out, sizeof(out), "@ok stat %s\n", json);
  write_all(out, n > 0 ? static_cast<size_t>(n) : 0);
}

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
  if (std::strcmp(line, "@fls") == 0) {
    handle_fls();
    return;
  }
  if (std::strcmp(line, "@reboot") == 0) {
    handle_reboot();
    return;
  }
  if (std::strcmp(line, "@stat") == 0) {
    handle_stat();
    return;
  }
  if (std::strcmp(line, "@adc") == 0) {
    handle_adc();
    return;
  }
  if (std::strcmp(line, "@gpin") == 0) {
    handle_gpin();
    return;
  }
  if (std::strncmp(line, "@gpinhist", 9) == 0 &&
      (line[9] == '\0' || line[9] == ' ')) {
    const char* arg = line[9] == ' ' ? line + 10 : line + 9;
    handle_gpinhist(arg);
    return;
  }
  {
    char path[128];
    unsigned long nbytes_ul;
    char crc_hex[16];
    if (std::sscanf(line, "@fput %127s %lu %15s", path, &nbytes_ul, crc_hex) == 3) {
      handle_fput(path, static_cast<uint32_t>(nbytes_ul), crc_hex);
      return;
    }
    if (std::sscanf(line, "@fget %127s", path) == 1) {
      handle_fget(path);
      return;
    }
    if (std::sscanf(line, "@fdel %127s", path) == 1) {
      handle_fdel(path);
      return;
    }
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
  // Live panel y-offset adjustment, for aligning the write window to the
  // glass without a reflash. Takes effect on the next full redraw.
  int y_gap;
  if (std::sscanf(line, "@gap %d", &y_gap) == 1) {
    if (g_gap_cb == nullptr) {
      reply_err("no gap handler registered");
    } else if (y_gap < 0 || y_gap > 64) {
      reply_err("gap out of range (0..64)");
    } else {
      g_gap_cb(y_gap);
      reply_ok("gap set");
    }
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

void init(void (*on_cal)(), void (*on_time_set)(int64_t), void (*on_gap)(int),
          void (*on_stat)(char*, size_t)) {
  g_cal_cb = on_cal;
  g_time_cb = on_time_set;
  g_gap_cb = on_gap;
  g_stat_cb = on_stat;
  usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  // The default 256-byte TX ring buffer cannot hold a @shot chunk (writes
  // larger than an item the ring can hold fail immediately, they don't block).
  cfg.tx_buffer_size = 8192;
  // The default 256-byte RX ring stalls sustained @fput streams: the host
  // pushes 4KB chunks and the ring drains through LittleFS writes, so give
  // the inbound side the same headroom as the outbound.
  cfg.rx_buffer_size = 8192;
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

  // 12KB, not the old 6KB: handle_fput keeps a 4KB chunk buffer on this
  // task's stack and LittleFS writes underneath it need headroom too.
  xTaskCreate(rx_task, "devctl_rx", 12288, nullptr, 3, nullptr);
  ESP_LOGI(TAG, "devctl ready (@ping, @shot, @tap, @swipe, @time, @gap, @stat, "
                "@fput, @fget, @fls, @fdel, @reboot, @adc, @gpin, @gpinhist)");
}

}  // namespace devctl
