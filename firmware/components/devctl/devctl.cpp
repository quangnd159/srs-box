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

#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
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
// Command dispatch

void (*g_cal_cb)() = nullptr;
void (*g_time_cb)(int64_t) = nullptr;
void (*g_gap_cb)(int) = nullptr;

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

void init(void (*on_cal)(), void (*on_time_set)(int64_t), void (*on_gap)(int)) {
  g_cal_cb = on_cal;
  g_time_cb = on_time_set;
  g_gap_cb = on_gap;
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
  ESP_LOGI(TAG, "devctl ready (@ping, @shot, @tap, @swipe, @time, @gap, "
                "@fput, @fget, @fls, @fdel, @reboot)");
}

}  // namespace devctl
