#!/usr/bin/env python3
"""Compile a word list into the on-device `.srs` deck format.

    deckc.py build --name "HSK 1" --id hsk1 decks/hsk1.raw.tsv -o decks/hsk1.srs
    deckc.py inspect decks/hsk1.srs

Input is the hskhsk.com tab-separated format:

    simplified <TAB> traditional <TAB> numeric-pinyin <TAB> pinyin <TAB> gloss

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
    with path.open(encoding="utf-8-sig") as fh:  # utf-8-sig strips the BOM
        for lineno, line in enumerate(fh, 1):
            line = line.rstrip("\n").rstrip("\r")
            if not line.strip():
                continue
            parts = line.split("\t")
            if len(parts) < 5:
                print(f"  skipping line {lineno}: expected 5 fields, got {len(parts)}",
                      file=sys.stderr)
                continue
            simplified, _traditional, _numeric, pinyin, gloss = parts[:5]
            simplified = simplified.strip()
            if not simplified:
                continue
            if simplified in seen:
                # Duplicate headwords would collide on card id.
                print(f"  skipping duplicate headword {simplified!r} on line {lineno}",
                      file=sys.stderr)
                continue
            seen.add(simplified)
            rows.append(
                dict(front=simplified, reading=pinyin.strip(), back=gloss.strip())
            )
    return rows


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
        reading = text.add(row["reading"])
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
        f"name={args.name}\nslug={args.id}\ncards={len(cards)}\n".encode("utf-8")
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

    glyphs = sorted({ch for r in rows for ch in (r["front"] + r["reading"] + r["back"])})
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
            return data[toff + o : toff + o + n].decode("utf-8") if n else ""

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
