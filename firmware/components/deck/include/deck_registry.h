// Multi-deck support: scans /data/decks/*.srs, parses each deck's META
// section, and computes per-deck queue counts by instantiating a throwaway
// Session and replaying the global review log through it.
//
// This works because of one fact documented in docs/sync-protocol.md: card
// ids embed the deck slug, so one global revlog serves every deck, and
// replaying the whole log into a deck's Session simply skips entries whose
// card id isn't in that deck (Session::index_of returns -1 for them, see
// session.h). Nothing deck-specific needs to be filtered out here.
//
// Header-only and POSIX-only (opendir/readdir, fopen/fseek), matching the
// style of deck.h/session.h/persist.h, so it runs unchanged on the device
// (LittleFS is exposed through the same VFS calls the IDF wires up) and in
// the host simulator/tests.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>

#include "deck.h"
#include "fsrs.h"
#include "session.h"

namespace deck {

// Mirrors the META keys tools/deckc.py writes (docs/sync-protocol.md):
// name=, slug=, cards=, lang=. `lang` is a recent addition the compiler
// defaults to "zh", so a deck compiled before it existed simply has no
// lang= line at all -- treat that exactly like lang=zh, never as an error.
struct Meta {
  std::string name;
  std::string slug;
  std::string lang = "zh";
  int cards = 0;
};

// META is newline-separated `key=value` lines, not null-terminated as a
// whole (see the CARD/TEXT sections for the same convention). Unknown keys
// are ignored so a newer compiler can add one without breaking this parser,
// the same "skip, don't fail" rule the section table itself follows.
inline Meta parse_meta(const char* data, uint32_t len) {
  Meta m;
  size_t i = 0;
  while (i < len) {
    size_t eol = i;
    while (eol < len && data[eol] != '\n') eol++;
    size_t eq = i;
    while (eq < eol && data[eq] != '=') eq++;
    if (eq < eol) {
      const std::string key(data + i, eq - i);
      const std::string val(data + eq + 1, eol - eq - 1);
      if (key == "name") m.name = val;
      else if (key == "slug") m.slug = val;
      else if (key == "lang" && !val.empty()) m.lang = val;
      else if (key == "cards") m.cards = std::atoi(val.c_str());
    }
    i = eol + 1;
  }
  return m;
}

struct DeckFile {
  std::string path;  // full path to the .srs file
  std::string slug;  // filename stem; used if META has none
};

// Lists every *.srs file in `dir`, sorted by filename for a stable picker
// order across boots. A missing directory is a normal fallback case (see
// docs/sync-protocol.md), not an error, so this returns empty rather than
// failing.
inline std::vector<DeckFile> scan(const char* dir) {
  std::vector<DeckFile> out;
  DIR* d = opendir(dir);
  if (!d) return out;
  struct dirent* ent;
  while ((ent = readdir(d)) != nullptr) {
    const std::string name = ent->d_name;
    constexpr size_t kExtLen = 4;  // ".srs"
    if (name.size() <= kExtLen) continue;
    if (name.compare(name.size() - kExtLen, kExtLen, ".srs") != 0) continue;
    DeckFile f;
    f.path = std::string(dir) + "/" + name;
    f.slug = name.substr(0, name.size() - kExtLen);
    out.push_back(f);
  }
  closedir(d);
  std::sort(out.begin(), out.end(),
            [](const DeckFile& a, const DeckFile& b) { return a.slug < b.slug; });
  return out;
}

// Reads a whole file into `out`. Decks are at most a few hundred KB (see
// docs/deck-format.md's font-size measurements for a sense of scale), so
// loading one fully is cheap; deck::Deck never copies out of it, so the
// buffer must outlive every Deck/Card view built from it.
inline bool read_file(const char* path, std::vector<uint8_t>* out) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size <= 0) {
    std::fclose(f);
    return false;
  }
  out->resize(static_cast<size_t>(size));
  const bool ok = std::fread(out->data(), 1, out->size(), f) == out->size();
  std::fclose(f);
  return ok;
}

// Replays `entries` into a throwaway Session over `d` and returns the
// resulting queue counts. One Session per deck, discarded right after --
// "hundreds of cards, cheap" is the whole justification for computing every
// deck's counts this way instead of maintaining separate persistent state
// per deck.
inline session::Counts counts_for(const Deck& d, const fsrs::Parameters& params,
                                  const session::Limits& limits,
                                  const std::vector<session::ReviewEntry>& entries,
                                  int64_t now, int utc_offset) {
  session::Session s(d, params, limits);
  s.replay(entries.data(), entries.size());
  return s.counts(now, utc_offset);
}

}  // namespace deck
