// On-flash persistence for the review log and the last-known wall-clock
// time, both described in CLAUDE.md and docs/deck-format.md.
//
// Header-only and free of ESP-IDF dependencies, in the same style as
// deck.h/fsrs.h/session.h, so the record format and recovery logic can be
// host-tested against a real temp directory rather than the device. The
// glue that actually mounts LittleFS lives in main.cpp; this component only
// knows about FILE* and paths.
//
// The review log is the source of truth (see session.h and
// docs/deck-format.md): every grade is appended here before the in-RAM
// schedule is trusted across a reboot. Boot replays every record through
// session::Session::replay() to reconstruct CardState from scratch.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "session.h"

namespace persist {

// ---------------------------------------------------------------------------
// revlog.bin: one 24-byte record per grade, matching docs/deck-format.md's
// ReviewEntry layout exactly (card_id, reviewed, rating, state, duration,
// reserved), preceded by a 12-byte header (magic + version) so a corrupt or
// foreign file is detected instead of misread as history.
//
// Fields are encoded byte-by-byte rather than via sizeof(struct): struct
// padding is a compiler decision and must never leak into an on-flash
// format (see deck.h's rd32/rd64 helpers for the same reasoning).

constexpr char kMagic[8] = {'S', 'R', 'S', 'R', 'L', 'O', 'G', '1'};
constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderBytes = 12;   // magic[8] + version(u32)
constexpr size_t kRecordBytes = 24;   // card_id(8) reviewed(8) rating(1) state(1) duration(2) reserved(4)

struct Entry {
  uint64_t card_id = 0;
  int64_t reviewed = 0;      // unix seconds
  uint8_t rating = 0;        // 1 Again, 2 Hard, 3 Good, 4 Easy
  uint8_t state_before = 0;  // card state before the review (0-3)
  uint16_t duration_ms = 0;
};

namespace detail {

inline void wr32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
inline void wr64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }
inline void wr16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
inline uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t rd64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline uint16_t rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

inline void encode(const Entry& e, uint8_t* out /* kRecordBytes */) {
  wr64(out + 0, e.card_id);
  wr64(out + 8, static_cast<uint64_t>(e.reviewed));
  out[16] = e.rating;
  out[17] = e.state_before;
  wr16(out + 18, e.duration_ms);
  wr32(out + 20, 0);  // reserved, per docs/deck-format.md; zeroed for future use
}

inline Entry decode(const uint8_t* in) {
  Entry e;
  e.card_id = rd64(in + 0);
  e.reviewed = static_cast<int64_t>(rd64(in + 8));
  e.rating = in[16];
  e.state_before = in[17];
  e.duration_ms = rd16(in + 18);
  // in[20..23] (reserved) intentionally ignored.
  return e;
}

}  // namespace detail

// Result of reading revlog.bin at boot.
struct LoadResult {
  std::vector<Entry> entries;
  bool file_existed = false;
  // false only for a bad/missing magic. Never set for a merely truncated
  // tail — that case still yields every complete record, see below.
  bool header_ok = true;
  // True when the file ended partway through a record, e.g. a power cut
  // mid-append. The partial bytes are dropped from `entries` but never
  // crash or discard the records before them.
  bool truncated_tail = false;
};

// Reads every complete record from `path`.
//
//  - Missing file: not an error, just "no history yet" (a normal first
//    boot). Returns an empty result with file_existed = false.
//  - Bad magic: the file exists but isn't a recognised revlog, or is
//    corrupt from the very first byte. Reported via header_ok = false with
//    no entries; the file itself is left untouched (never wiped) so it
//    remains available for manual recovery.
//  - Short trailing record: the most recent append never finished. Every
//    complete record before it is kept; the caller should follow up with
//    truncate_to_complete_records() so the next append starts from a clean
//    boundary instead of growing a corrupt tail forever.
inline LoadResult load(const char* path) {
  LoadResult result;
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return result;  // no file yet; fresh start
  result.file_existed = true;

  uint8_t header[kHeaderBytes];
  if (std::fread(header, 1, kHeaderBytes, f) != kHeaderBytes ||
      std::memcmp(header, kMagic, sizeof(kMagic)) != 0) {
    result.header_ok = false;
    std::fclose(f);
    return result;
  }

  uint8_t rec[kRecordBytes];
  size_t n;
  while ((n = std::fread(rec, 1, kRecordBytes, f)) == kRecordBytes) {
    result.entries.push_back(detail::decode(rec));
  }
  if (n != 0) result.truncated_tail = true;  // short read: partial record at EOF
  std::fclose(f);
  return result;
}

// Drops any bytes past the last complete record (see LoadResult::truncated_tail
// above). A no-op if the file is already aligned or does not exist. Relies on
// POSIX truncate(), which esp_littlefs implements through the VFS layer;
// failure here is non-fatal (see main.cpp's `ok()` policy) — the caller just
// logs and moves on, since the worst case is one misaligned append, not
// data loss.
inline bool truncate_to_complete_records(const char* path) {
  std::FILE* probe = std::fopen(path, "rb");
  if (!probe) return true;  // nothing to truncate
  std::fseek(probe, 0, SEEK_END);
  const long size = std::ftell(probe);
  std::fclose(probe);
  if (size < static_cast<long>(kHeaderBytes)) return true;  // header itself short/missing; leave alone

  const long body = size - static_cast<long>(kHeaderBytes);
  const long complete_bytes =
      (body / static_cast<long>(kRecordBytes)) * static_cast<long>(kRecordBytes);
  const long want = static_cast<long>(kHeaderBytes) + complete_bytes;
  if (want == size) return true;  // already aligned

#if defined(_WIN32)
  return false;  // not needed on the device; host tests run on POSIX
#else
  return ::truncate(path, want) == 0;
#endif
}

// Appends one record to `path`, writing the header first if the file is
// being created for the first time. Refuses to append onto a file with a
// bad header rather than risk interleaving well-formed records with
// garbage; the caller should log this and continue with RAM-only state for
// the rest of the boot (see main.cpp's `ok()` comment on the same policy
// for hardware calls).
inline bool append(const char* path, const Entry& e) {
  bool need_header = true;
  if (std::FILE* probe = std::fopen(path, "rb")) {
    uint8_t header[kHeaderBytes];
    const bool magic_ok = std::fread(header, 1, kHeaderBytes, probe) == kHeaderBytes &&
                          std::memcmp(header, kMagic, sizeof(kMagic)) == 0;
    std::fclose(probe);
    if (!magic_ok) return false;  // corrupt header; do not touch the file further
    need_header = false;
  }

  std::FILE* out = std::fopen(path, need_header ? "wb" : "ab");
  if (!out) return false;

  if (need_header) {
    uint8_t header[kHeaderBytes];
    std::memcpy(header, kMagic, sizeof(kMagic));
    detail::wr32(header + 8, kVersion);
    if (std::fwrite(header, 1, kHeaderBytes, out) != kHeaderBytes) {
      std::fclose(out);
      return false;
    }
  }

  uint8_t rec[kRecordBytes];
  detail::encode(e, rec);
  const bool ok = std::fwrite(rec, 1, kRecordBytes, out) == kRecordBytes;
  std::fflush(out);
  std::fclose(out);
  return ok;
}

// Reviews-today count derived from the log itself rather than a RAM counter,
// so it survives a reboot for free. Reuses session::day_index and its 4am
// rollover rule (see session.h) with the same UTC offset main.cpp uses
// elsewhere, so "today" means the same thing everywhere in the firmware.
inline int reviewed_today(const std::vector<Entry>& entries, int64_t now,
                          int utc_offset_seconds) {
  const int64_t today = session::day_index(now, utc_offset_seconds);
  int n = 0;
  for (const auto& e : entries) {
    if (session::day_index(e.reviewed, utc_offset_seconds) == today) ++n;
  }
  return n;
}

// ---------------------------------------------------------------------------
// lastknowntime.bin: a single 12-byte record (magic + unix seconds) updated
// on every grade and periodically from the heartbeat loop. There is no RTC
// on this board (see CLAUDE.md), so on boot, if the system clock reads as
// unset, this is the only thing standing between "cards read as due later
// than they should be" (safe: time simply didn't advance while the device
// was off) and "cards read as due in 1970" (every card looks new).

constexpr char kTimeMagic[4] = {'S', 'R', 'T', 'M'};
constexpr size_t kTimeRecordBytes = 12;  // magic[4] + seconds(i64)

inline bool save_time(const char* path, int64_t unix_seconds) {
  std::FILE* f = std::fopen(path, "wb");  // whole-file rewrite; this is one small record
  if (!f) return false;
  uint8_t buf[kTimeRecordBytes];
  std::memcpy(buf, kTimeMagic, sizeof(kTimeMagic));
  detail::wr64(buf + 4, static_cast<uint64_t>(unix_seconds));
  const bool ok = std::fwrite(buf, 1, sizeof(buf), f) == sizeof(buf);
  std::fclose(f);
  return ok;
}

inline bool load_time(const char* path, int64_t* out_seconds) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  uint8_t buf[kTimeRecordBytes];
  const bool ok = std::fread(buf, 1, sizeof(buf), f) == sizeof(buf) &&
                  std::memcmp(buf, kTimeMagic, sizeof(kTimeMagic)) == 0;
  if (ok) *out_seconds = static_cast<int64_t>(detail::rd64(buf + 4));
  std::fclose(f);
  return ok;
}

}  // namespace persist
