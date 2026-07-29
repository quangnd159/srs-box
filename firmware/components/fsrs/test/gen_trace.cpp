// Drives the C++ FSRS-6 port over deterministic pseudo-random review
// sequences and prints a trace. tools/check_fsrs.py replays the identical
// sequences through py-fsrs and diffs the result.
//
//   c++ -std=c++17 -O2 -I../include gen_trace.cpp -o gen_trace && ./gen_trace

#include <cinttypes>
#include <cstdio>
#include <string>

#include "fsrs.h"

namespace {

// xorshift64*, so the sequence is identical everywhere without depending on
// the host's <random> implementation.
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed) {}
  uint64_t next() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 2685821657736338717ULL;
  }
  uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }
};

}  // namespace

int main(int argc, char** argv) {
  fsrs::Parameters p;
  // Diffing against py-fsrs requires matching its same-day floor threshold;
  // see the comment on short_term_floor_from_hard.
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--pyfsrs-compat") {
      p.short_term_floor_from_hard = false;
    }
  }
  const int64_t epoch = 1750000000;  // arbitrary fixed unix time

  std::printf("case\tstep\trating\telapsed_days\tstability\tdifficulty\tstate\treps\tlapses\n");

  Rng rng(0x5152535455565758ULL);
  for (int c = 0; c < 200; ++c) {
    fsrs::Card card;
    int64_t now = epoch;

    for (int step = 0; step < 12; ++step) {
      const auto rating = static_cast<fsrs::Rating>(1 + rng.below(4));

      // Mix same-day reviews with gaps up to ~three months so the short-term
      // branch, the lapse branch, and long-interval recall all get exercised.
      int64_t gap_days;
      const uint32_t roll = rng.below(10);
      if (roll < 3) {
        gap_days = 0;
      } else if (roll < 8) {
        gap_days = 1 + rng.below(30);
      } else {
        gap_days = 1 + rng.below(90);
      }
      now += gap_days * 86400;

      const int64_t elapsed = fsrs::elapsed_days(card.last_review, now);
      const auto result = fsrs::review(p, card, rating, now);
      card = result.card;

      std::printf("%d\t%d\t%d\t%" PRId64 "\t%.10f\t%.10f\t%d\t%u\t%u\n", c, step,
                  static_cast<int>(rating), elapsed, card.memory.stability,
                  card.memory.difficulty, static_cast<int>(card.state),
                  card.reps, card.lapses);
    }
  }
  return 0;
}
