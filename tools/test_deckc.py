#!/usr/bin/env python3
"""Compiler tests: pinyin syllable segmentation and the reading field.

    python3 tools/test_deckc.py

Run by tools/runtests.sh. The TypeScript twin's equivalents live in
web/test/pinyinSegment.test.ts, and web/test/deckc.acceptance.test.ts
byte-compares the two compilers' output.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from deckc import SYLLABLE_SEP, numeric_syllables, reading_field, segment_reading  # noqa: E402

SEP = SYLLABLE_SEP
failures = 0


def check(label, got, want):
    global failures
    if got != want:
        failures += 1
        print(f"  FAIL {label}\n    got  {got!r}\n    want {want!r}")


def check_raises(label, fn, fragment):
    global failures
    try:
        fn()
    except ValueError as exc:
        if fragment not in str(exc):
            failures += 1
            print(f"  FAIL {label}: {exc!r} does not mention {fragment!r}")
        return
    except SystemExit as exc:
        if fragment not in str(exc):
            failures += 1
            print(f"  FAIL {label}: {exc!r} does not mention {fragment!r}")
        return
    failures += 1
    print(f"  FAIL {label}: did not raise")


# --- numeric_syllables ------------------------------------------------------
check("splits on tone digits", numeric_syllables("you3shi2hou5"), ["you", "shi", "hou"])
check("ü spelling", numeric_syllables("lü3you2"), ["lv", "you"])
check("u: spelling", numeric_syllables("lu:3you2"), ["lv", "you"])
check("v spelling", numeric_syllables("lv3you2"), ["lv", "you"])
check("erhua is a suffix", numeric_syllables("nü3hai2r5"), ["nv", "hair"])
check_raises("letters with no tone", lambda: numeric_syllables("zhen1de"), "no tone digit")

# --- segment_reading --------------------------------------------------------
# The bug this exists for: scanning tone marks alone gives "yǒ|ushí|hou".
check("coda stays with its syllable", segment_reading("yǒushíhou", "you3shi2hou5"),
      f"yǒu{SEP}shí{SEP}hou")
check("neutral syllable is its own run", segment_reading("bàba", "ba4ba5"), f"bà{SEP}ba")
check("ü in the reading", segment_reading("lǚyóu", "lü3you2"), f"lǚ{SEP}yóu")
check("u: in the numeric column", segment_reading("hūlüè", "hu1lu:e4"), f"hū{SEP}lüè")
check("erhua", segment_reading("nǚháir", "nü3hai2r5"), f"nǚ{SEP}háir")
check("alternate readings", segment_reading("cháng, zhǎng", "chang2, zhang3"),
      f"cháng{SEP}, zhǎng")
check("empty reading", segment_reading("", "ai4"), "")
check("empty numeric", segment_reading("ài", ""), "ài")

check_raises("leftover letters", lambda: segment_reading("nǎa", "na3"), "unconsumed")
check_raises("reading too short", lambda: segment_reading("nǐhǎo", "ni3hao3ma5"),
             "ends before syllable")
check_raises("mismatched letters", lambda: segment_reading("nǐhǎo", "ni3ma3"), "does not match")

# --- reading_field ----------------------------------------------------------
row = dict(front="有时候", reading="yǒushíhou", back="sometimes", numeric="you3shi2hou5", lineno=1)
src = Path("test.tsv")
check("zh segments", reading_field(row, "zh", src), f"yǒu{SEP}shí{SEP}hou")
check("non-zh passes through", reading_field(row, "fr", src), "yǒushíhou")
check("no numeric column passes through",
      reading_field({**row, "numeric": ""}, "zh", src), "yǒushíhou")
check_raises("a misaligned row fails the compile",
             lambda: reading_field({**row, "numeric": "you3shi2"}, "zh", src),
             "cannot align pinyin syllables")

if failures:
    print(f"{failures} deckc test(s) FAILED")
    sys.exit(1)
print("deckc segmentation tests passed")
