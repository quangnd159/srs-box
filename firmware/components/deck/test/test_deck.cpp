// Reads a deck the Python compiler actually produced and checks the C++
// reader agrees with it. Writer and reader are independent implementations
// of docs/deck-format.md, so this catches drift between them.
//
//   c++ -std=c++17 -O2 -I../include test_deck.cpp -o test_deck
//   ./test_deck ../../../../decks/hsk1-2.srs

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "deck.h"

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (cond) {
    std::printf("  ok    %s\n", what.c_str());
  } else {
    std::printf("  FAIL  %s\n", what.c_str());
    g_failures++;
  }
}

std::string to_string(const deck::Str& s) {
  return s.data ? std::string(s.data, s.len) : std::string();
}

std::vector<uint8_t> read_file(const char* path) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::printf("cannot open %s\n", path);
    std::exit(2);
  }
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> buf(static_cast<size_t>(n));
  if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
    std::printf("short read on %s\n", path);
    std::exit(2);
  }
  std::fclose(f);
  return buf;
}

}  // namespace

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "decks/hsk1-2.srs";
  auto bytes = read_file(path);
  std::printf("deck: %s (%zu bytes)\n", path, bytes.size());

  deck::Deck d;
  const auto err = d.open(bytes.data(), bytes.size(), /*verify_crc=*/true);
  check(err == deck::Error::None,
        std::string("opens cleanly: ") + deck::error_string(err));
  if (err != deck::Error::None) return 1;

  // 301, not 300: the source word list had 坐 and 吧 fused onto one line by a
  // missing trailing newline, so 吧 was silently dropped until it was recovered.
  check(d.count() == 301, "card count is 301, got " + std::to_string(d.count()));
  check(d.ids_ascending(), "card ids strictly ascending (binary search is valid)");

  // Every card must have a headword and a gloss; reading may be absent.
  size_t missing_front = 0, missing_back = 0, missing_reading = 0;
  for (size_t i = 0; i < d.count(); ++i) {
    const auto c = d.at(i);
    if (c.front.empty()) missing_front++;
    if (c.back.empty()) missing_back++;
    if (c.reading.empty()) missing_reading++;
  }
  check(missing_front == 0, "every card has a headword");
  check(missing_back == 0, "every card has a gloss");
  std::printf("  note  %zu cards without pinyin\n", missing_reading);

  // Round-trip: every id the reader reports must be findable by binary search.
  size_t lookup_failures = 0;
  for (size_t i = 0; i < d.count(); ++i) {
    const auto c = d.at(i);
    deck::Card found;
    if (!d.find(c.id, &found) || found.id != c.id ||
        to_string(found.front) != to_string(c.front)) {
      lookup_failures++;
    }
  }
  check(lookup_failures == 0, "every card is findable by id");

  check(!d.find(0xDEADBEEFCAFEULL, nullptr), "absent id is not found");

  // UTF-8 sanity: headwords should be multi-byte CJK, not mojibake.
  size_t cjk_headwords = 0;
  for (size_t i = 0; i < d.count(); ++i) {
    const auto s = to_string(d.at(i).front);
    if (!s.empty() && static_cast<unsigned char>(s[0]) >= 0x80) cjk_headwords++;
  }
  check(cjk_headwords == d.count(), "all headwords are non-ASCII (CJK intact)");

  std::printf("--- first 5 cards as the device will read them ---\n");
  for (size_t i = 0; i < 5 && i < d.count(); ++i) {
    const auto c = d.at(i);
    std::printf("  %-8s %-14s %s\n", to_string(c.front).c_str(),
                to_string(c.reading).c_str(), to_string(c.back).c_str());
  }

  // Corruption must be detected, not silently tolerated.
  {
    auto tampered = bytes;
    tampered[tampered.size() / 2] ^= 0xFF;
    deck::Deck t;
    check(t.open(tampered.data(), tampered.size(), true) == deck::Error::BadCrc,
          "a flipped byte is caught by crc32");
  }
  {
    auto truncated = bytes;
    truncated.resize(truncated.size() - 16);
    deck::Deck t;
    check(t.open(truncated.data(), truncated.size(), true) ==
              deck::Error::SizeMismatch,
          "a truncated transfer is rejected");
  }
  {
    auto bad = bytes;
    bad[0] = 'X';
    deck::Deck t;
    check(t.open(bad.data(), bad.size(), true) == deck::Error::BadMagic,
          "a non-deck file is rejected");
  }

  std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASS\n", g_failures);
  return g_failures ? 1 : 0;
}
