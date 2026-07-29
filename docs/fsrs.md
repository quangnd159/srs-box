# FSRS-6 implementation notes

`firmware/components/fsrs/include/fsrs.h` is a header-only C++ port of FSRS-6, ported from py-fsrs (MIT) and fsrs-rs (BSD-3-Clause), both © Open Spaced Repetition. Deliberately **not** ported from Anki's `rslib`, which is AGPL-3.0.

Header-only and free of ESP-IDF dependencies, so the identical code runs on the device and under host tests.

## Verification

`test/gen_trace.cpp` drives 200 cards through 12 reviews each — 2,400 steps — using a seeded xorshift so the sequence is reproducible anywhere. Gaps are mixed deliberately: same-day reviews, one-to-thirty-day gaps, and up-to-ninety-day gaps, so the short-term branch, the lapse branch, and long-interval recall all get exercised. `test/check_fsrs.py` replays the identical rating/elapsed-day sequences through py-fsrs and compares stability, difficulty, and state at every step.

```bash
c++ -std=c++17 -O2 -I../include gen_trace.cpp -o gen_trace
./gen_trace --pyfsrs-compat > trace.tsv
uv run --with fsrs python check_fsrs.py trace.tsv
# compared 2400 review steps
# PASS — C++ port matches py-fsrs exactly
```

Rerun this after touching anything in `fsrs.h`. It catches the whole class of silent scheduling bugs that otherwise surface months later as wasted study time.

## The one intentional divergence

The two reference implementations have drifted apart on the same-day stability floor, so this is a decision, not a bug:

- **fsrs-rs** `model.rs:283` — `last_s * if rating >= 2.0 { sinc.max(1.0) } else { sinc }`, flooring for **Hard and above**.
- **py-fsrs 6.3.1** (current release) `_short_term_stability` — floors only for **Good and Easy**.

We follow **fsrs-rs**, because Anki schedules with the fsrs-rs crate and this device's review log is meant to reproduce Anki's scheduling. Diverging here would mean the stick and the desktop disagree about the same card.

`Parameters::short_term_floor_from_hard` defaults to `true` (fsrs-rs). The test flips it to `false` only to isolate this known difference and prove every other part of the port is faithful. Ship with the default.

The practical effect: on a same-day Hard, fsrs-rs holds stability flat while py-fsrs lets it fall. Compounded over a relearning streak the two diverge by a factor of two or more.

## Details that exist only in source code

These appear in no published description of FSRS and are each load-bearing. All three were confirmed by reading the reference implementations directly.

**The mean-reversion target is unclamped.** `next_difficulty` reverts toward `D_0(4) = w4 - e^(3·w5) + 1`, which with default weights is about **-4.77**. Difficulty is clamped to `[1, 10]` everywhere else, and clamping it here too is the single most common porting bug. It silently weakens mean reversion for every card.

**Lapses have a stability cap.** Post-lapse stability is `min(PLS, S / e^(w17·w18))`. Without the cap, a lapse can *increase* stability — forgetting a card would make the scheduler more confident about it.

**Stability uses the pre-update difficulty.** `stability_after_success` and `stability_after_failure` both take the old `D`, so retrievability and stability must be computed before difficulty is updated. Reordering these produces plausible-looking numbers that are quietly wrong.

## Things that did *not* change in FSRS-6

Worth recording, because secondary write-ups imply otherwise. Difficulty updating — linear damping and mean reversion toward `D_0(4)` — landed during FSRS-5's lifetime, not in FSRS-6. The FSRS-6 diff leaves `next_difficulty`, `linear_damping`, `mean_reversion`, and `init_difficulty` byte-identical.

FSRS-6's actual changes are narrow: 19 → 21 parameters, a learnable decay at `w[20]` (FSRS-5 hardcoded `-0.5`), a new `S^(-w19)` term in same-day stability, `S_MIN` lowered from 0.01 to 0.001, and a retuned default weight vector.

An FSRS-5 parameter set upgrades by appending `[0.0, 0.5]`, which reproduces FSRS-5 exactly.

## Numeric notes

py-fsrs is f64; fsrs-rs is f32 throughout. This port uses `double`, matching py-fsrs, which is why the differential test agrees to well under `1e-6`. Bit-exact agreement with fsrs-rs would require f32 and is not a goal — Anki recomputes from the review log anyway, and the log is what we sync.

`std::round` is half-away-from-zero while Python's `round` is half-to-even. Intervals are integers derived from a continuous function, so exact `.5` values are vanishingly rare, but it is a real difference if a mismatch ever shows up at a single day.
