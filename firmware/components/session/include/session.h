// Review session: decides which card you see next, and records what you said.
//
// Header-only and hardware-free so the queue policy can be tested against
// simulated study days on the host.
//
// The design rule from docs/deck-format.md holds here: the review log is the
// source of truth and card state is a checkpoint derived from it. Grading
// appends to the log first, then updates state. A power cut between the two
// costs nothing, because replaying the log rebuilds the state.

#pragma once

#include <cstdint>
#include <vector>

#include "deck.h"
#include "fsrs.h"

namespace session {

// Mirrors the on-disk record in docs/deck-format.md.
struct CardState {
  uint64_t id = 0;
  int64_t due = 0;          // unix seconds
  float stability = 0.0f;
  float difficulty = 0.0f;
  int64_t last_review = 0;  // 0 = never reviewed
  uint16_t reps = 0;
  uint16_t lapses = 0;
  uint8_t state = 0;        // 0 new, 1 learning, 2 review, 3 relearning
  uint8_t step = 0;

  bool is_new() const { return last_review == 0; }
};

struct ReviewEntry {
  uint64_t card_id = 0;
  int64_t reviewed = 0;
  uint8_t rating = 0;
  uint8_t state_before = 0;
  uint16_t duration_ms = 0;
};

struct Limits {
  // Anki's defaults are 20 new and 200 reviews. On a handheld the session is
  // shorter by nature, so these are lower; both are per day.
  int new_per_day = 15;
  int reviews_per_day = 120;
};

struct Counts {
  int learning = 0;
  int due = 0;
  int fresh = 0;  // "new" is a keyword-adjacent name; this is unseen cards

  int total() const { return learning + due + fresh; }
  bool empty() const { return total() == 0; }
};

// Start-of-day boundary. Anki rolls the day over at 4am rather than midnight,
// so a late-night session still counts as the previous day. Worth copying:
// studying at 1am should not silently consume tomorrow's new cards.
constexpr int kDayRolloverHour = 4;

inline int64_t day_index(int64_t unix_seconds, int utc_offset_seconds) {
  const int64_t local = unix_seconds + utc_offset_seconds -
                        static_cast<int64_t>(kDayRolloverHour) * 3600;
  // Floor division, so days before the epoch do not round toward zero.
  return local >= 0 ? local / 86400 : -(((-local) + 86399) / 86400);
}

class Session {
 public:
  Session(const deck::Deck& d, const fsrs::Parameters& params, Limits limits)
      : params_(params), limits_(limits) {
    state_.resize(d.count());
    for (size_t i = 0; i < d.count(); ++i) {
      state_[i].id = d.at(i).id;
    }
  }

  // Replays a review log over the current state. This is how state is rebuilt
  // after a crash, and how a freshly synced deck picks up existing history.
  // Entries must be in chronological order.
  void replay(const ReviewEntry* entries, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      const int index = index_of(entries[i].card_id);
      if (index < 0) continue;  // a card removed from the deck; ignore
      apply(index, static_cast<fsrs::Rating>(entries[i].rating),
            entries[i].reviewed);
    }
  }

  Counts counts(int64_t now, int utc_offset) const {
    Counts c;
    const int64_t today = day_index(now, utc_offset);
    int reviews_left = limits_.reviews_per_day - reviews_done_on(today);
    int new_left = limits_.new_per_day - new_done_on(today);

    for (const auto& s : state_) {
      if (s.is_new()) {
        if (new_left > 0) {
          c.fresh++;
          new_left--;
        }
      } else if (s.state == 1 || s.state == 3) {
        if (s.due <= now) c.learning++;
      } else if (s.due <= now) {
        if (reviews_left > 0) {
          c.due++;
          reviews_left--;
        }
      }
    }
    return c;
  }

  // Index of the next card to show, or -1 when the session is finished.
  //
  // Priority: learning cards that are due, then reviews, then new cards.
  // Learning comes first because those intervals are minutes long and a card
  // waiting on a 1-minute step is genuinely more urgent than a review that
  // has been waiting a week.
  int next_card(int64_t now, int utc_offset) const {
    const int64_t today = day_index(now, utc_offset);

    int best = -1;
    int64_t best_due = 0;
    for (size_t i = 0; i < state_.size(); ++i) {
      const auto& s = state_[i];
      if (s.is_new() || (s.state != 1 && s.state != 3)) continue;
      if (s.due > now) continue;
      if (best < 0 || s.due < best_due) {
        best = static_cast<int>(i);
        best_due = s.due;
      }
    }
    if (best >= 0) return best;

    if (reviews_done_on(today) < limits_.reviews_per_day) {
      for (size_t i = 0; i < state_.size(); ++i) {
        const auto& s = state_[i];
        if (s.is_new() || s.state != 2 || s.due > now) continue;
        if (best < 0 || s.due < best_due) {
          best = static_cast<int>(i);
          best_due = s.due;
        }
      }
      if (best >= 0) return best;
    }

    if (new_done_on(today) < limits_.new_per_day) {
      for (size_t i = 0; i < state_.size(); ++i) {
        if (state_[i].is_new()) return static_cast<int>(i);
      }
    }
    return -1;
  }

  // What each button would do, for the interval labels on the grading UI.
  // Anki shows these and they matter: they are how you learn to grade
  // honestly rather than mashing Good.
  int64_t preview_interval(int index, fsrs::Rating rating, int64_t now) const {
    if (index < 0 || static_cast<size_t>(index) >= state_.size()) return 0;
    const auto card = to_fsrs(state_[index]);
    return fsrs::review(params_, card, rating, now).interval_seconds;
  }

  // Records a grade. Returns the log entry the caller must persist *before*
  // trusting the updated state.
  ReviewEntry grade(int index, fsrs::Rating rating, int64_t now,
                    uint16_t duration_ms = 0) {
    ReviewEntry e;
    if (index < 0 || static_cast<size_t>(index) >= state_.size()) return e;
    e.card_id = state_[index].id;
    e.reviewed = now;
    e.rating = static_cast<uint8_t>(rating);
    e.state_before = state_[index].state;
    e.duration_ms = duration_ms;

    apply(index, rating, now);
    log_.push_back(e);
    return e;
  }

  const CardState& state_at(int index) const { return state_[index]; }
  const std::vector<CardState>& all_state() const { return state_; }
  const std::vector<ReviewEntry>& log() const { return log_; }

  int index_of(uint64_t id) const {
    // state_ mirrors the deck order, which is sorted by id.
    size_t lo = 0, hi = state_.size();
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      if (state_[mid].id == id) return static_cast<int>(mid);
      if (state_[mid].id < id) lo = mid + 1; else hi = mid;
    }
    return -1;
  }

 private:
  static fsrs::Card to_fsrs(const CardState& s) {
    fsrs::Card c;
    c.memory.stability = s.stability;
    c.memory.difficulty = s.difficulty;
    c.memory.initialized = !s.is_new();
    c.state = static_cast<fsrs::State>(s.state == 0 ? 1 : s.state);
    c.step = s.step;
    c.due = s.due;
    c.last_review = s.last_review;
    c.reps = s.reps;
    c.lapses = s.lapses;
    return c;
  }

  void apply(int index, fsrs::Rating rating, int64_t now) {
    auto& s = state_[index];
    const auto result = fsrs::review(params_, to_fsrs(s), rating, now);
    const auto& c = result.card;
    s.stability = static_cast<float>(c.memory.stability);
    s.difficulty = static_cast<float>(c.memory.difficulty);
    s.state = static_cast<uint8_t>(c.state);
    s.step = static_cast<uint8_t>(c.step < 0 ? 0 : c.step);
    s.due = c.due;
    s.last_review = c.last_review;
    s.reps = c.reps;
    s.lapses = c.lapses;
  }

  // Daily caps count what was actually reviewed that day, taken from the log
  // rather than a counter, so they survive reboots for free.
  int reviews_done_on(int64_t day) const {
    int n = 0;
    for (const auto& e : log_) {
      if (day_index(e.reviewed, utc_offset_) == day && e.state_before == 2) n++;
    }
    return n;
  }

  int new_done_on(int64_t day) const {
    int n = 0;
    for (const auto& e : log_) {
      if (day_index(e.reviewed, utc_offset_) == day && e.state_before == 0) n++;
    }
    return n;
  }

  fsrs::Parameters params_;
  Limits limits_;
  int utc_offset_ = 7 * 3600;  // Vietnam, UTC+7
  std::vector<CardState> state_;
  std::vector<ReviewEntry> log_;
};

}  // namespace session
