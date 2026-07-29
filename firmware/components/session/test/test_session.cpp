// Simulates study days against a real compiled deck and checks the queue
// policy and the crash-recovery invariant.
//
//   c++ -std=c++17 -O2 -I../include -I../../deck/include -I../../fsrs/include \
//       test_session.cpp -o test_session && ./test_session ../../../../decks/hsk1-2.srs

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "session.h"

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  std::printf(cond ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
  if (!cond) g_failures++;
}

std::vector<uint8_t> read_file(const char* path) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) { std::printf("cannot open %s\n", path); std::exit(2); }
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> buf(static_cast<size_t>(n));
  if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) std::exit(2);
  std::fclose(f);
  return buf;
}

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed) {}
  uint64_t next() {
    s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
    return s * 2685821657736338717ULL;
  }
  uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }
};

constexpr int64_t kStart = 1780000000;  // arbitrary fixed unix time

}  // namespace

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "decks/hsk1-2.srs";
  auto bytes = read_file(path);

  deck::Deck d;
  if (d.open(bytes.data(), bytes.size()) != deck::Error::None) {
    std::printf("deck failed to open\n");
    return 2;
  }
  std::printf("deck: %zu cards\n", d.count());

  const fsrs::Parameters params;
  session::Limits limits;  // 15 new/day, 120 reviews/day

  // --- a fresh deck offers only new cards, capped -------------------------
  {
    session::Session s(d, params, limits);
    const auto c = s.counts(kStart, 7 * 3600);
    check(c.fresh == limits.new_per_day,
          "fresh deck offers exactly new_per_day cards, got " +
              std::to_string(c.fresh));
    check(c.due == 0 && c.learning == 0, "fresh deck has nothing due");
    check(s.next_card(kStart, 7 * 3600) >= 0, "fresh deck has a next card");
  }

  // --- the daily new-card cap is actually enforced -------------------------
  {
    session::Session s(d, params, limits);
    int64_t now = kStart;
    int introduced = 0;
    for (int i = 0; i < 200; ++i) {
      const int idx = s.next_card(now, 7 * 3600);
      if (idx < 0) break;
      if (s.state_at(idx).is_new()) introduced++;
      // Always Easy, so cards graduate immediately and stop coming back.
      s.grade(idx, fsrs::Rating::Easy, now);
      now += 20;  // 20 seconds per card
    }
    check(introduced == limits.new_per_day,
          "no more than new_per_day new cards in one day, got " +
              std::to_string(introduced));
  }

  // --- learning cards are served before reviews ----------------------------
  {
    session::Session s(d, params, limits);
    int64_t now = kStart;
    // Answer one card Again so it enters learning with a 1-minute step.
    const int first = s.next_card(now, 7 * 3600);
    s.grade(first, fsrs::Rating::Again, now);
    now += 120;  // past the 1-minute step
    const int next = s.next_card(now, 7 * 3600);
    check(next == first,
          "a lapsed card in learning is served again before anything else");
  }

  // --- crash recovery: replaying the log rebuilds identical state ----------
  {
    session::Session live(d, params, limits);
    Rng rng(0xA1B2C3D4E5F60718ULL);
    int64_t now = kStart;

    // Simulate 30 days of somewhat realistic grading.
    for (int day = 0; day < 30; ++day) {
      for (int i = 0; i < 400; ++i) {
        const int idx = live.next_card(now, 7 * 3600);
        if (idx < 0) break;
        // Mostly Good, sometimes Again/Hard/Easy.
        const uint32_t r = rng.below(10);
        fsrs::Rating rating = fsrs::Rating::Good;
        if (r == 0) rating = fsrs::Rating::Again;
        else if (r == 1) rating = fsrs::Rating::Hard;
        else if (r == 2) rating = fsrs::Rating::Easy;
        live.grade(idx, rating, now);
        now += 15;
      }
      now = kStart + static_cast<int64_t>(day + 1) * 86400;
    }

    const auto& log = live.log();
    std::printf("  note  simulated %zu reviews over 30 days\n", log.size());
    check(log.size() > 200, "the simulation actually did meaningful work");

    session::Session rebuilt(d, params, limits);
    rebuilt.replay(log.data(), log.size());

    size_t mismatches = 0;
    for (size_t i = 0; i < d.count(); ++i) {
      const auto& a = live.all_state()[i];
      const auto& b = rebuilt.all_state()[i];
      if (a.id != b.id || a.due != b.due || a.reps != b.reps ||
          a.lapses != b.lapses || a.state != b.state ||
          a.stability != b.stability || a.difficulty != b.difficulty) {
        mismatches++;
      }
    }
    check(mismatches == 0,
          "replaying the review log reproduces state exactly (" +
              std::to_string(mismatches) + " mismatches)");

    // Scheduling should have spread cards into the future, not piled up.
    int scheduled_ahead = 0, seen = 0;
    for (const auto& st : live.all_state()) {
      if (!st.is_new()) {
        seen++;
        if (st.due > now) scheduled_ahead++;
      }
    }
    std::printf("  note  %d cards seen, %d scheduled beyond day 30\n", seen,
                scheduled_ahead);
    check(seen > 0 && scheduled_ahead > 0, "cards get scheduled into the future");
  }

  // --- day rollover is at 4am, not midnight --------------------------------
  {
    const int off = 7 * 3600;
    // Build from a day index so the constants are unambiguous: local midnight
    // is exactly D*86400 in local seconds, hence D*86400 - off in unix time.
    const int64_t D = 20455;
    const int64_t local_midnight = D * 86400 - off;
    const int64_t one_am = local_midnight + 1 * 3600;
    const int64_t three_59 = local_midnight + 4 * 3600 - 60;
    const int64_t five_am = local_midnight + 5 * 3600;

    check(session::day_index(one_am, off) == D - 1,
          "1am local belongs to the previous study day");
    check(session::day_index(three_59, off) == D - 1,
          "3:59am local is still the previous study day");
    check(session::day_index(five_am, off) == D,
          "5am local is the new study day");
    check(session::day_index(one_am, off) + 1 == session::day_index(five_am, off),
          "the study day rolls over at 4am, so a 1am session counts as yesterday");
  }

  std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASS\n", g_failures);
  return g_failures ? 1 : 0;
}
