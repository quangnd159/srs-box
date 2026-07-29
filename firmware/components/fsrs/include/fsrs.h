// FSRS-6 (Free Spaced Repetition Scheduler, version 6).
//
// Ported from the py-fsrs (MIT) and fsrs-rs (BSD-3-Clause) reference
// implementations, both (c) Open Spaced Repetition. Deliberately not ported
// from Anki's rslib, which is AGPL.
//
// Header-only and free of ESP-IDF dependencies so the same code compiles for
// the device and for host tests that diff against py-fsrs.
//
// Several details below exist only in the reference source and appear in no
// published description of the algorithm. They are load-bearing; each is
// marked. See docs/fsrs.md.

#pragma once

#include <cmath>
#include <cstdint>

namespace fsrs {

enum class Rating : uint8_t { Again = 1, Hard = 2, Good = 3, Easy = 4 };

// There is no New state. A card never reviewed is Learning at step 0 with
// stability and difficulty still unset, which `Memory::initialized` tracks.
enum class State : uint8_t { Learning = 1, Review = 2, Relearning = 3 };

constexpr double kStabilityMin = 0.001;
constexpr double kStabilityMax = 36500.0;
constexpr double kDifficultyMin = 1.0;
constexpr double kDifficultyMax = 10.0;
constexpr int32_t kMaximumIntervalDays = 36500;

struct Parameters {
  // FSRS-6 default weights, retuned in fsrs-rs PR #337.
  double w[21] = {0.212,  1.2931, 2.3065, 8.2956, 6.4133, 0.8334, 3.0194,
                  0.001,  1.8722, 0.1666, 0.7960, 1.4835, 0.0614, 0.2629,
                  1.6483, 0.6014, 1.8729, 0.5425, 0.0912, 0.0658, 0.1542};

  double desired_retention = 0.9;

  // Learning and relearning steps, in seconds. Matches py-fsrs defaults.
  int32_t learning_steps[4] = {60, 600, 0, 0};
  int32_t learning_step_count = 2;
  int32_t relearning_steps[4] = {600, 0, 0, 0};
  int32_t relearning_step_count = 1;

  // The two reference implementations have diverged here, so this is a real
  // choice rather than a tunable. fsrs-rs (model.rs: `if rating >= 2.0`)
  // floors the same-day stability increase at 1.0 for Hard and above;
  // py-fsrs 6.3.1, the current release, floors it only for Good and Easy.
  //
  // Default follows fsrs-rs, because Anki schedules with the fsrs-rs crate
  // and this device's review log is meant to reproduce Anki's scheduling.
  // The host test flips this to false to diff against py-fsrs and confirm
  // every other part of the port is bit-faithful.
  bool short_term_floor_from_hard = true;

  // DECAY is learnable in FSRS-6; in FSRS-5 it was hardcoded to -0.5.
  double decay() const { return -w[20]; }

  // Chosen so that R(S, S) == 0.9 exactly, whatever the decay.
  double factor() const { return std::pow(0.9, 1.0 / decay()) - 1.0; }
};

struct Memory {
  double stability = 0.0;
  double difficulty = 0.0;
  bool initialized = false;
};

struct Card {
  Memory memory;
  State state = State::Learning;
  int32_t step = 0;          // index into the active step list; -1 in Review
  int64_t due = 0;           // unix seconds
  int64_t last_review = 0;   // unix seconds; 0 means never reviewed
  uint16_t reps = 0;
  uint16_t lapses = 0;
};

namespace detail {

inline double clamp(double v, double lo, double hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

inline double clamp_stability(double s) {
  return clamp(s, kStabilityMin, kStabilityMax);
}

inline double initial_stability(const Parameters& p, Rating g) {
  return clamp_stability(p.w[static_cast<int>(g) - 1]);
}

// `clamp_result` is false only for the mean-reversion target in
// next_difficulty. D_0(4) evaluates to about -4.77 with default weights, and
// the reference implementations feed that raw negative value into mean
// reversion. Clamping it to 1.0 there is the most common porting bug.
inline double initial_difficulty(const Parameters& p, Rating g,
                                 bool clamp_result = true) {
  const double d = p.w[4] - std::exp(p.w[5] * (static_cast<int>(g) - 1)) + 1.0;
  return clamp_result ? clamp(d, kDifficultyMin, kDifficultyMax) : d;
}

inline double next_difficulty(const Parameters& p, double difficulty,
                              Rating g) {
  const double delta = -p.w[6] * (static_cast<int>(g) - 3);
  // Linear damping: moves difficulty less as it approaches the ceiling.
  const double damped = difficulty + delta * (10.0 - difficulty) / 9.0;
  // Mean reversion toward the (unclamped) easy-grade initial difficulty.
  const double reverted =
      p.w[7] * initial_difficulty(p, Rating::Easy, false) + (1.0 - p.w[7]) * damped;
  return clamp(reverted, kDifficultyMin, kDifficultyMax);
}

inline double stability_after_success(const Parameters& p, double d, double s,
                                      double r, Rating g) {
  const double hard_penalty = (g == Rating::Hard) ? p.w[15] : 1.0;
  const double easy_bonus = (g == Rating::Easy) ? p.w[16] : 1.0;
  const double inc = 1.0 + std::exp(p.w[8]) * (11.0 - d) * std::pow(s, -p.w[9]) *
                               (std::exp(p.w[10] * (1.0 - r)) - 1.0) *
                               hard_penalty * easy_bonus;
  return clamp_stability(s * inc);
}

inline double stability_after_failure(const Parameters& p, double d, double s,
                                      double r) {
  const double pls = p.w[11] * std::pow(d, -p.w[12]) *
                     (std::pow(s + 1.0, p.w[13]) - 1.0) *
                     std::exp(p.w[14] * (1.0 - r));
  // Post-lapse stability cap. Present only in the reference source, never in
  // the published write-ups. Without it a lapse can raise stability.
  const double cap = s / std::exp(p.w[17] * p.w[18]);
  return clamp_stability(std::fmin(pls, cap));
}

inline double stability_short_term(const Parameters& p, double s, Rating g) {
  const double inc =
      std::exp(p.w[17] * (static_cast<int>(g) - 3 + p.w[18])) * std::pow(s, -p.w[19]);
  const bool floored = p.short_term_floor_from_hard ? (g >= Rating::Hard)
                                                    : (g >= Rating::Good);
  const double applied = floored ? std::fmax(inc, 1.0) : inc;
  return clamp_stability(s * applied);
}

}  // namespace detail

// Probability of recall after `elapsed_days` with the given stability.
inline double retrievability(const Parameters& p, double stability,
                             int64_t elapsed_days) {
  if (stability <= 0.0 || elapsed_days < 0) return 1.0;
  return std::pow(
      1.0 + p.factor() * static_cast<double>(elapsed_days) / stability,
      p.decay());
}

// Days until retrievability decays to the desired retention.
inline int32_t next_interval_days(const Parameters& p, double stability) {
  const double raw = (stability / p.factor()) *
                     (std::pow(p.desired_retention, 1.0 / p.decay()) - 1.0);
  const double rounded = std::round(raw);
  if (rounded < 1.0) return 1;
  if (rounded > kMaximumIntervalDays) return kMaximumIntervalDays;
  return static_cast<int32_t>(rounded);
}

// Elapsed whole days between two instants, floored at zero.
inline int64_t elapsed_days(int64_t last_review, int64_t now) {
  if (last_review <= 0 || now <= last_review) return 0;
  return (now - last_review) / 86400;
}

struct ReviewResult {
  Card card;
  int64_t interval_seconds = 0;
};

// Apply a grade. `now` is unix seconds. The returned card is the updated one;
// the input is left untouched so callers can preview each rating's outcome to
// show intervals on the grading buttons.
inline ReviewResult review(const Parameters& p, const Card& in, Rating g,
                           int64_t now) {
  Card c = in;
  const int64_t days = elapsed_days(c.last_review, now);
  const bool same_day = (c.last_review > 0) && (days < 1);

  // --- memory state ---------------------------------------------------------
  if (!c.memory.initialized) {
    c.memory.stability = detail::initial_stability(p, g);
    c.memory.difficulty = detail::initial_difficulty(p, g);
    c.memory.initialized = true;
  } else {
    const double r = retrievability(p, c.memory.stability, days);
    const double old_d = c.memory.difficulty;
    // Stability uses the pre-update difficulty, so it must be computed first.
    if (same_day) {
      c.memory.stability = detail::stability_short_term(p, c.memory.stability, g);
    } else if (g == Rating::Again) {
      c.memory.stability =
          detail::stability_after_failure(p, old_d, c.memory.stability, r);
    } else {
      c.memory.stability =
          detail::stability_after_success(p, old_d, c.memory.stability, r, g);
    }
    c.memory.difficulty = detail::next_difficulty(p, old_d, g);
  }

  // --- state machine --------------------------------------------------------
  const int32_t* steps;
  int32_t step_count;
  if (c.state == State::Review) {
    steps = p.relearning_steps;
    step_count = p.relearning_step_count;
  } else if (c.state == State::Relearning) {
    steps = p.relearning_steps;
    step_count = p.relearning_step_count;
  } else {
    steps = p.learning_steps;
    step_count = p.learning_step_count;
  }

  int64_t interval = 0;

  auto graduate = [&]() {
    c.state = State::Review;
    c.step = -1;
    interval = static_cast<int64_t>(next_interval_days(p, c.memory.stability)) * 86400;
  };

  if (c.state == State::Review) {
    if (g == Rating::Again) {
      c.lapses++;
      if (step_count == 0) {
        interval =
            static_cast<int64_t>(next_interval_days(p, c.memory.stability)) * 86400;
      } else {
        c.state = State::Relearning;
        c.step = 0;
        interval = steps[0];
      }
    } else {
      interval =
          static_cast<int64_t>(next_interval_days(p, c.memory.stability)) * 86400;
    }
  } else {
    // Learning or Relearning.
    if (step_count == 0 || (c.step >= step_count && g >= Rating::Hard)) {
      graduate();
    } else {
      switch (g) {
        case Rating::Again:
          c.step = 0;
          interval = steps[0];
          break;
        case Rating::Hard:
          if (c.step == 0 && step_count == 1) {
            interval = static_cast<int64_t>(steps[0] * 1.5);
          } else if (c.step == 0 && step_count >= 2) {
            interval = (steps[0] + steps[1]) / 2;
          } else {
            interval = steps[c.step];
          }
          break;
        case Rating::Good:
          if (c.step + 1 == step_count) {
            graduate();
          } else {
            c.step += 1;
            interval = steps[c.step];
          }
          break;
        case Rating::Easy:
          graduate();
          break;
      }
    }
  }

  c.reps++;
  c.last_review = now;
  c.due = now + interval;
  return {c, interval};
}

}  // namespace fsrs
