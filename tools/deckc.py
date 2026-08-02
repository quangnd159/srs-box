#!/usr/bin/env python3
"""Compile a word list into the on-device `.srs` deck format.

    deckc.py build --name "HSK 1" --id hsk1 decks/hsk1.raw.tsv -o decks/hsk1.srs
    deckc.py build --name "French 1" --id fr1 --lang fr decks/fr1.tsv -o decks/fr1.srs
    deckc.py inspect decks/hsk1.srs

Input is tab-separated, in one of two shapes, auto-detected from the first
data row's column count (a single file must be entirely one shape or the
other):

    simplified <TAB> traditional <TAB> numeric-pinyin <TAB> pinyin <TAB> gloss

  the hskhsk.com format, 5 columns, used for Chinese decks; or

    front <TAB> reading <TAB> back

  a 3-column format for everything else (reading may be empty, e.g. a
  language with no separate phonetic transcription).

`--lang` (default "zh") is written to the deck's META section and gates
language-specific rendering on-device: pinyin tone-colouring of the reading
line applies only when lang=zh (see docs/sync-protocol.md).

For zh decks the reading is emitted with an ASCII unit separator (0x1F)
between syllables, reconstructed from the numeric-pinyin column, so the
device does not have to guess where syllables end. See `segment_reading`.

See docs/deck-format.md for the binary layout. The compiler is also
responsible for reporting the glyph set, which the font subsetter consumes.
"""
import argparse
import hashlib
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b"SRSDECK1"
HEADER_FMT = "<8sIIQI4x"  # magic, total_size, section_count, deck_id, crc32, pad
HEADER_SIZE = struct.calcsize(HEADER_FMT)
SECTION_FMT = "<4sIII"
SECTION_SIZE = struct.calcsize(SECTION_FMT)
CARD_FMT = "<QIHIHIHHI"
CARD_SIZE = struct.calcsize(CARD_FMT)

assert HEADER_SIZE == 32, HEADER_SIZE
assert SECTION_SIZE == 16, SECTION_SIZE
assert CARD_SIZE == 32, CARD_SIZE


def stable_id(deck_slug: str, headword: str) -> int:
    """64-bit card id, stable across recompiles.

    Derived from the deck slug and the headword only, never from row order,
    so reordering or inserting rows preserves scheduling state. Changing a
    headword deliberately creates a new card; see docs/deck-format.md.
    """
    digest = hashlib.sha256(f"{deck_slug}\x00{headword}".encode()).digest()
    # Top bit cleared so the value stays positive if it ever lands in a
    # signed integer on the host side.
    return int.from_bytes(digest[:8], "little") & 0x7FFFFFFFFFFFFFFF


# ---------------------------------------------------------------------------
# Pinyin syllable segmentation.
#
# The display reading ("yǒushíhou") carries tone marks but no syllable
# boundaries, and a syllable's coda sits *after* its tone mark, so the device
# cannot recover the boundaries by scanning for tone marks alone: "yǒushíhou"
# would come out as "yǒ|ushí|hou". The boundaries are however already in the
# source TSV, in the numeric-pinyin column ("you3shi2hou5"), so the compiler
# does the split once and records it.
#
# The result is written into the reading field with U+001F (ASCII unit
# separator) between syllables. It is a control character, so it can never
# collide with reading text, never reaches a font subset, and old firmware
# that ignores it degrades to the old heuristic rather than mis-rendering.
SYLLABLE_SEP = "\x1f"

# Tone-marked vowels folded to their base letter, so a numeric syllable
# ("lve") can be compared against the display letters ("lüè"). ü and its
# toned forms fold to "v", matching the "v"/"u:" spellings the numeric
# column uses for it.
_FOLD = {
    ch: base
    for base, marked in (
        ("a", "āáǎà"),
        ("e", "ēéěè"),
        ("i", "īíǐì"),
        ("o", "ōóǒò"),
        ("u", "ūúǔù"),
        ("v", "ǖǘǚǜü"),
    )
    for ch in marked
}

# Characters that are part of a reading but not part of a syllable: the
# comma-space between alternate readings ("cháng, zhǎng"), the syllable
# apostrophe ("Xī'ān"), hyphens and the middle dot.
_GLUE = " ,'-·"


def _fold(ch: str) -> str:
    c = ch.lower()
    return _FOLD.get(c, c)


def numeric_syllables(numeric: str) -> list[str]:
    """Splits a numeric-pinyin string into folded, digit-free syllables.

    "you3shi2hou5" -> ["you", "shi", "hou"]. Raises ValueError on anything
    it cannot account for, because a guessed split is worse than no split.
    """
    out: list[str] = []
    cur = ""
    i = 0
    while i < len(numeric):
        ch = numeric[i]
        if ch.isdigit():
            if ch not in "12345" or not cur:
                raise ValueError(f"stray tone digit {ch!r}")
            out.append(cur)
            cur = ""
        elif ch.isascii() and ch.isalpha():
            f = _fold(ch)
            if f == "u" and numeric[i + 1 : i + 2] == ":":
                f = "v"  # "lu:e4", the ASCII spelling of "lüe4"
                i += 1
            cur += f
        elif ch == "ü" or ch == "Ü":
            cur += "v"
        elif ch in _GLUE:
            pass
        else:
            raise ValueError(f"unexpected character {ch!r} in numeric pinyin")
        i += 1
    if cur:
        raise ValueError(f"syllable {cur!r} has no tone digit")
    # Erhua: a trailing "r" is a suffix on the preceding syllable, not a
    # syllable of its own, so "hai2r5" is one run ("háir"), not two.
    merged: list[str] = []
    for syl in out:
        if syl == "r" and merged:
            merged[-1] += "r"
        else:
            merged.append(syl)
    return merged


def segment_reading(reading: str, numeric: str) -> str:
    """Returns `reading` with SYLLABLE_SEP inserted at syllable boundaries.

    Alignment is by folded letters: each numeric syllable consumes exactly
    that many letters of the display reading, and glue characters (see
    _GLUE) ride along without being counted. Raises ValueError if the two
    columns disagree anywhere; the caller must fail the compile rather than
    emit a guess.
    """
    if not reading or not numeric:
        return reading
    syllables = numeric_syllables(numeric)
    if not syllables:
        raise ValueError("no syllables in the numeric pinyin")

    segments: list[str] = []
    i = 0
    for syl in syllables:
        seg = ""
        j = 0
        while j < len(syl):
            if i >= len(reading):
                raise ValueError(f"reading ends before syllable {syl!r}")
            ch = reading[i]
            if ch.isalpha():
                if _fold(ch) != syl[j]:
                    raise ValueError(f"{ch!r} does not match {syl[j]!r} of {syl!r}")
                j += 1
            elif ch in _GLUE:
                if j != 0:
                    raise ValueError(f"{ch!r} splits syllable {syl!r}")
            else:
                raise ValueError(f"unexpected character {ch!r} in the reading")
            seg += ch
            i += 1
        segments.append(seg)

    while i < len(reading):  # trailing glue joins the last syllable
        ch = reading[i]
        if ch.isalpha():
            raise ValueError(f"unconsumed reading text {reading[i:]!r}")
        if ch not in _GLUE:
            raise ValueError(f"unexpected character {ch!r} in the reading")
        segments[-1] += ch
        i += 1

    return SYLLABLE_SEP.join(segments)


class TextPool:
    """Deduplicating UTF-8 blob. Cards sharing a gloss share bytes."""

    def __init__(self) -> None:
        self.buf = bytearray()
        self.offsets: dict[str, tuple[int, int]] = {}

    def add(self, s: str) -> tuple[int, int]:
        if not s:
            return (0, 0)
        if s in self.offsets:
            return self.offsets[s]
        raw = s.encode("utf-8")
        if len(raw) > 0xFFFF:
            raise ValueError(f"string too long for a u16 length: {s[:40]!r}")
        entry = (len(self.buf), len(raw))
        self.buf += raw
        self.offsets[s] = entry
        return entry


def read_rows(path: Path) -> list[dict]:
    rows = []
    seen: set[str] = set()
    expected_cols: int | None = None  # set from the first data row; see module docstring
    with path.open(encoding="utf-8-sig") as fh:  # utf-8-sig strips the BOM
        for lineno, line in enumerate(fh, 1):
            line = line.rstrip("\n").rstrip("\r")
            if not line.strip():
                continue
            parts = line.split("\t")

            if expected_cols is None:
                # First data row decides the shape for the whole file: 3
                # columns (front/reading/back) or 5 (hskhsk.com's format).
                expected_cols = 3 if len(parts) < 5 else 5

            if len(parts) < expected_cols:
                print(f"  skipping line {lineno}: expected {expected_cols} fields, got "
                      f"{len(parts)}", file=sys.stderr)
                continue

            if expected_cols == 5:
                front, _traditional, numeric, reading, back = parts[:5]
            else:
                front, reading, back = parts[:3]
                numeric = ""

            front = front.strip()
            if not front:
                continue
            if front in seen:
                # Duplicate headwords would collide on card id.
                print(f"  skipping duplicate headword {front!r} on line {lineno}",
                      file=sys.stderr)
                continue
            seen.add(front)
            rows.append(
                dict(
                    front=front,
                    reading=reading.strip(),
                    back=back.strip(),
                    numeric=numeric.strip(),
                    lineno=lineno,
                )
            )
    return rows


def reading_field(row: dict, lang: str, src: Path) -> str:
    """The reading as stored: syllable-separated for zh, verbatim otherwise.

    Only lang "zh" is tone-coloured on-device (see review_ui.cpp set_lang),
    and only the 5-column shape carries a numeric-pinyin column to segment
    with, so everything else passes through untouched and byte-identically.
    """
    if lang != "zh" or not row["numeric"] or not row["reading"]:
        return row["reading"]
    try:
        return segment_reading(row["reading"], row["numeric"])
    except ValueError as exc:
        sys.exit(
            f"{src}:{row['lineno']}: cannot align pinyin syllables: {exc}\n"
            f"  {row['front']}\t{row['numeric']}\t{row['reading']}\n"
            f"  fix the numeric-pinyin or reading column; the compiler will "
            f"not guess a split"
        )


def build(args) -> int:
    src = Path(args.source)
    rows = read_rows(src)
    if not rows:
        sys.exit(f"no usable rows in {src}")

    text = TextPool()
    cards = []
    for row in rows:
        front = text.add(row["front"])
        back = text.add(row["back"])
        reading = text.add(reading_field(row, args.lang, src))
        cards.append(
            dict(
                id=stable_id(args.id, row["front"]),
                front=front,
                back=back,
                reading=reading,
            )
        )

    # The device binary-searches this section, so it must be sorted by id.
    cards.sort(key=lambda c: c["id"])
    ids = [c["id"] for c in cards]
    if len(set(ids)) != len(ids):
        sys.exit("card id collision; this should be impossible with sha256")

    card_blob = b"".join(
        struct.pack(
            CARD_FMT,
            c["id"],
            c["front"][0], c["front"][1],
            c["back"][0], c["back"][1],
            c["reading"][0], c["reading"][1],
            0,  # tags
            0,  # reserved
        )
        for c in cards
    )

    meta_blob = (
        f"name={args.name}\nslug={args.id}\ncards={len(cards)}\nlang={args.lang}\n"
        .encode("utf-8")
    )

    sections = [(b"META", meta_blob), (b"CARD", card_blob), (b"TEXT", bytes(text.buf))]

    body_start = HEADER_SIZE + SECTION_SIZE * len(sections)
    table = b""
    body = b""
    offset = body_start
    for sid, blob in sections:
        table += struct.pack(SECTION_FMT, sid, offset, len(blob), 0)
        body += blob
        offset += len(blob)

    payload = table + body
    total_size = HEADER_SIZE + len(payload)
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    header = struct.pack(
        HEADER_FMT,
        MAGIC,
        total_size,
        len(sections),
        stable_id(args.id, "\x00deck"),
        crc,
    )

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(header + payload)

    # Collected from the source text, never from the emitted reading: the
    # syllable separator is a control character and must not reach the font
    # subsetter, which would ask lv_font_conv for a glyph that doesn't exist.
    glyphs = sorted(
        {
            ch
            for r in rows
            for ch in (r["front"] + r["reading"] + r["back"])
            if ch >= " "
        }
    )
    cjk = [g for g in glyphs if "一" <= g <= "鿿"]

    print(f"{out}  {total_size:,} bytes")
    print(f"  cards        {len(cards)}")
    print(f"  text pool    {len(text.buf):,} bytes ({len(text.offsets)} unique strings)")
    print(f"  glyphs       {len(glyphs)} total, {len(cjk)} CJK")
    if args.glyphs:
        Path(args.glyphs).write_text("".join(glyphs), encoding="utf-8")
        print(f"  glyph set    {args.glyphs}")
    return 0


def inspect(args) -> int:
    data = Path(args.file).read_bytes()
    magic, total_size, section_count, deck_id, crc = struct.unpack_from(
        HEADER_FMT, data, 0
    )
    if magic != MAGIC:
        sys.exit(f"bad magic {magic!r}")
    ok_size = total_size == len(data)
    ok_crc = (zlib.crc32(data[HEADER_SIZE:]) & 0xFFFFFFFF) == crc
    print(f"magic {magic.decode()}  deck_id 0x{deck_id:016x}")
    print(f"size  {total_size:,} {'ok' if ok_size else f'MISMATCH (file is {len(data):,})'}")
    print(f"crc32 0x{crc:08x} {'ok' if ok_crc else 'MISMATCH'}")

    sections = {}
    for i in range(section_count):
        sid, off, length, _flags = struct.unpack_from(
            SECTION_FMT, data, HEADER_SIZE + i * SECTION_SIZE
        )
        sections[sid] = (off, length)
        print(f"  section {sid.decode()}  offset {off:>8}  length {length:>8}")

    if b"META" in sections:
        off, length = sections[b"META"]
        print("--- meta ---")
        print(data[off : off + length].decode("utf-8").rstrip())

    if b"CARD" in sections and b"TEXT" in sections:
        coff, clen = sections[b"CARD"]
        toff, _ = sections[b"TEXT"]

        def s(o: int, n: int) -> str:
            if not n:
                return ""
            # Syllable separators are shown as a middle dot so a compiled
            # reading's splits are visible here.
            return data[toff + o : toff + o + n].decode("utf-8").replace(SYLLABLE_SEP, "·")

        n = clen // CARD_SIZE
        print(f"--- {n} cards, first {min(args.limit, n)} ---")
        prev = -1
        for i in range(n):
            (cid, fo, fl, bo, bl, ro, rl, _tags, _res) = struct.unpack_from(
                CARD_FMT, data, coff + i * CARD_SIZE
            )
            if cid <= prev:
                sys.exit(f"card ids not strictly ascending at index {i}")
            prev = cid
            if i < args.limit:
                print(f"  {s(fo,fl):<6} {s(ro,rl):<12} {s(bo,bl)}")
        print(f"  (ids verified strictly ascending across all {n} cards)")
    return 0 if (ok_size and ok_crc) else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("build")
    b.add_argument("source")
    b.add_argument("-o", "--output", required=True)
    b.add_argument("--name", required=True)
    b.add_argument("--id", required=True, help="stable deck slug; changing it resets all card ids")
    b.add_argument("--lang", default="zh", help="bcp47-ish tag written to META (default zh)")
    b.add_argument("--glyphs", help="also write the glyph set to this file")
    b.set_defaults(func=build)

    i = sub.add_parser("inspect")
    i.add_argument("file")
    i.add_argument("--limit", type=int, default=10)
    i.set_defaults(func=inspect)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
