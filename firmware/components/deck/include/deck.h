// Reader for the `.srs` deck container produced by tools/deckc.py.
//
// Header-only and free of ESP-IDF dependencies so it can be host-tested
// against decks the Python compiler actually emits. See docs/deck-format.md
// for the layout and for why content and scheduling state are separate.
//
// Nothing here allocates or copies. Sections are read in place, which lets
// the device point straight at memory-mapped flash and keep a 300-card deck
// resident for the cost of a few pointers.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace deck {

constexpr char kMagic[8] = {'S', 'R', 'S', 'D', 'E', 'C', 'K', '1'};

// Text fields are not null-terminated; length always comes from the record.
struct Str {
  const char* data = nullptr;
  uint16_t len = 0;

  bool empty() const { return len == 0; }
};

struct Card {
  uint64_t id = 0;
  Str front;    // headword, e.g. the hanzi
  Str back;     // gloss
  Str reading;  // pinyin; may be empty
  uint16_t tags = 0;
};

enum class Error {
  None,
  TooSmall,
  BadMagic,
  SizeMismatch,
  BadCrc,
  MissingSection,
  Malformed,
};

inline const char* error_string(Error e) {
  switch (e) {
    case Error::None: return "ok";
    case Error::TooSmall: return "file smaller than a header";
    case Error::BadMagic: return "not an SRSDECK1 file";
    case Error::SizeMismatch: return "declared size does not match the data";
    case Error::BadCrc: return "crc32 mismatch (corrupt or truncated transfer)";
    case Error::MissingSection: return "required section missing";
    case Error::Malformed: return "section offsets out of range";
  }
  return "unknown";
}

namespace detail {

inline uint32_t rd32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;  // both host and ESP32-S3 are little-endian
}

inline uint64_t rd64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}

inline uint16_t rd16(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return v;
}

// Bitwise CRC-32 (IEEE), matching Python's zlib.crc32. No lookup table:
// this runs once at load, and 1KB of table is not worth it here.
inline uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) {
      crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1) + 1));
    }
  }
  return ~crc;
}

}  // namespace detail

class Deck {
 public:
  static constexpr size_t kHeaderSize = 32;
  static constexpr size_t kSectionSize = 16;
  static constexpr size_t kCardSize = 32;

  // `data` must outlive the Deck. Nothing is copied.
  //
  // `verify_crc` is worth paying on a freshly synced deck and worth skipping
  // on every boot thereafter: it walks the whole file.
  Error open(const uint8_t* data, size_t size, bool verify_crc = true) {
    reset();
    if (size < kHeaderSize) return Error::TooSmall;
    if (std::memcmp(data, kMagic, sizeof(kMagic)) != 0) return Error::BadMagic;

    const uint32_t total = detail::rd32(data + 8);
    const uint32_t section_count = detail::rd32(data + 12);
    deck_id_ = detail::rd64(data + 16);
    const uint32_t crc = detail::rd32(data + 24);

    if (total != size) return Error::SizeMismatch;
    if (verify_crc &&
        detail::crc32(data + kHeaderSize, size - kHeaderSize) != crc) {
      return Error::BadCrc;
    }

    if (kHeaderSize + section_count * kSectionSize > size) return Error::Malformed;

    const uint8_t* cards = nullptr;
    uint32_t cards_len = 0;
    for (uint32_t i = 0; i < section_count; ++i) {
      const uint8_t* e = data + kHeaderSize + i * kSectionSize;
      const uint32_t off = detail::rd32(e + 4);
      const uint32_t len = detail::rd32(e + 8);
      // Guard against a truncated or hostile file before dereferencing.
      if (off > size || len > size || off + len > size) return Error::Malformed;

      if (std::memcmp(e, "CARD", 4) == 0) {
        cards = data + off;
        cards_len = len;
      } else if (std::memcmp(e, "TEXT", 4) == 0) {
        text_ = data + off;
        text_len_ = len;
      } else if (std::memcmp(e, "META", 4) == 0) {
        meta_ = reinterpret_cast<const char*>(data + off);
        meta_len_ = len;
      }
      // Unknown sections are skipped, so a newer compiler can add sections
      // without breaking older firmware.
    }

    if (cards == nullptr || text_ == nullptr) return Error::MissingSection;
    if (cards_len % kCardSize != 0) return Error::Malformed;

    cards_ = cards;
    count_ = cards_len / kCardSize;
    return Error::None;
  }

  size_t count() const { return count_; }
  uint64_t deck_id() const { return deck_id_; }
  const char* meta() const { return meta_; }
  uint32_t meta_len() const { return meta_len_; }
  bool is_open() const { return cards_ != nullptr; }

  Card at(size_t index) const {
    Card c;
    if (index >= count_) return c;
    const uint8_t* p = cards_ + index * kCardSize;
    c.id = detail::rd64(p);
    c.front = str(detail::rd32(p + 8), detail::rd16(p + 12));
    c.back = str(detail::rd32(p + 14), detail::rd16(p + 18));
    c.reading = str(detail::rd32(p + 20), detail::rd16(p + 24));
    c.tags = detail::rd16(p + 26);
    return c;
  }

  // Records are sorted by id, so this is a binary search straight over flash
  // with no in-RAM index.
  bool find(uint64_t id, Card* out) const {
    size_t lo = 0, hi = count_;
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      const uint64_t mid_id = detail::rd64(cards_ + mid * kCardSize);
      if (mid_id == id) {
        if (out) *out = at(mid);
        return true;
      }
      if (mid_id < id) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return false;
  }

  // Confirms the invariant find() depends on. Cheap; run it after a sync.
  bool ids_ascending() const {
    for (size_t i = 1; i < count_; ++i) {
      if (detail::rd64(cards_ + i * kCardSize) <=
          detail::rd64(cards_ + (i - 1) * kCardSize)) {
        return false;
      }
    }
    return true;
  }

 private:
  void reset() {
    cards_ = nullptr;
    text_ = nullptr;
    meta_ = nullptr;
    count_ = 0;
    text_len_ = 0;
    meta_len_ = 0;
    deck_id_ = 0;
  }

  Str str(uint32_t off, uint16_t len) const {
    Str s;
    if (len == 0 || off + len > text_len_) return s;
    s.data = reinterpret_cast<const char*>(text_ + off);
    s.len = len;
    return s;
  }

  const uint8_t* cards_ = nullptr;
  const uint8_t* text_ = nullptr;
  const char* meta_ = nullptr;
  size_t count_ = 0;
  uint32_t text_len_ = 0;
  uint32_t meta_len_ = 0;
  uint64_t deck_id_ = 0;
};

}  // namespace deck
