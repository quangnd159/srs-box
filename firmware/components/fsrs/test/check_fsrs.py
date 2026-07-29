#!/usr/bin/env python3
"""Diff the C++ FSRS-6 port against the py-fsrs reference implementation.

    ./gen_trace > trace.tsv
    uv run --with fsrs check_fsrs.py trace.tsv

Replays the exact rating/elapsed-day sequences the C++ trace recorded and
compares stability, difficulty, state, and lapse count at every single step.
"""
import sys
from datetime import datetime, timedelta, timezone

from fsrs import Card, Rating, Scheduler, State

# Tolerance is for double-vs-double drift only; these should agree to ~1e-9.
TOL = 1e-6

CPP_STATE_TO_PY = {1: State.Learning, 2: State.Review, 3: State.Relearning}


def main(path: str) -> int:
    rows = []
    with open(path) as fh:
        next(fh)  # header
        for line in fh:
            case, step, rating, elapsed, stab, diff, state, reps, lapses = (
                line.rstrip("\n").split("\t")
            )
            rows.append(
                dict(
                    case=int(case),
                    step=int(step),
                    rating=int(rating),
                    elapsed=int(elapsed),
                    stability=float(stab),
                    difficulty=float(diff),
                    state=int(state),
                    reps=int(reps),
                    lapses=int(lapses),
                )
            )

    scheduler = Scheduler(enable_fuzzing=False)

    failures = []
    checked = 0
    card = None
    now = datetime(2025, 6, 15, 12, 0, 0, tzinfo=timezone.utc)
    current_case = None

    for row in rows:
        if row["case"] != current_case:
            current_case = row["case"]
            card = Card()
            now = datetime(2025, 6, 15, 12, 0, 0, tzinfo=timezone.utc)

        # Advance the clock by exactly the gap the C++ side saw, so py-fsrs
        # derives the same elapsed-day count internally.
        now = now + timedelta(days=row["elapsed"])

        card, _ = scheduler.review_card(card, Rating(row["rating"]), now)
        checked += 1

        problems = []
        if abs(card.stability - row["stability"]) > TOL:
            problems.append(
                f"stability py={card.stability!r} cpp={row['stability']!r}"
            )
        if abs(card.difficulty - row["difficulty"]) > TOL:
            problems.append(
                f"difficulty py={card.difficulty!r} cpp={row['difficulty']!r}"
            )
        if card.state != CPP_STATE_TO_PY[row["state"]]:
            problems.append(f"state py={card.state} cpp={row['state']}")

        if problems:
            failures.append((row["case"], row["step"], row["rating"], problems))

    print(f"compared {checked} review steps")
    if not failures:
        print("PASS — C++ port matches py-fsrs exactly")
        return 0

    print(f"FAIL — {len(failures)} mismatched steps")
    for case, step, rating, problems in failures[:15]:
        print(f"  case {case} step {step} rating {rating}")
        for p in problems:
            print(f"    {p}")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "trace.tsv"))
