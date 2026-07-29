#!/usr/bin/env bash
# All host-side tests. These need no hardware, so they should always pass.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fail=0

echo "=== FSRS-6 vs py-fsrs reference ==="
cd "$ROOT/firmware/components/fsrs/test"
c++ -std=c++17 -O2 -I../include gen_trace.cpp -o /tmp/gen_trace
/tmp/gen_trace --pyfsrs-compat > /tmp/fsrs_trace.tsv
uv run --with fsrs python check_fsrs.py /tmp/fsrs_trace.tsv || fail=1

echo
echo "=== deck reader vs compiled deck ==="
cd "$ROOT/firmware/components/deck/test"
c++ -std=c++17 -O2 -Wall -Wextra -I../include test_deck.cpp -o /tmp/test_deck
/tmp/test_deck "$ROOT/decks/hsk1-2.srs" || fail=1

echo
echo "=== review session and queue policy ==="
cd "$ROOT/firmware/components/session/test"
c++ -std=c++17 -O2 -Wall -Wextra \
    -I../include -I../../deck/include -I../../fsrs/include \
    test_session.cpp -o /tmp/test_session
/tmp/test_session "$ROOT/decks/hsk1-2.srs" || fail=1

echo
if [ $fail -eq 0 ]; then echo "ALL HOST TESTS PASSED"; else echo "SOME TESTS FAILED" >&2; fi
exit $fail
