import { describe, expect, test } from "bun:test";
import { buildDeck } from "../lib/compiler/build";
import { parseDeck } from "../lib/compiler/parse-srs";
import type { DeckRow } from "../lib/compiler/types";

const ROWS: DeckRow[] = [
  { front: "爱", reading: "ài", back: "yêu; yêu thích", numeric: "ai4" },
  { front: "有时候", reading: "yǒushíhou", back: "sometimes", numeric: "you3shi2hou5" },
  { front: "chat", reading: "ʃa", back: "cat" },
];

describe("parseDeck: round-trips buildDeck output", () => {
  test("recovers header, meta, and cards", async () => {
    const built = await buildDeck(ROWS, { name: "Test Deck", slug: "test-deck", lang: "zh" });
    const parsed = parseDeck(built.bytes);

    expect(parsed.header.crc32).toBeGreaterThan(0);
    expect(parsed.meta.name).toBe("Test Deck");
    expect(parsed.meta.slug).toBe("test-deck");
    expect(parsed.meta.lang).toBe("zh");
    expect(parsed.meta.cards).toBe(String(ROWS.length));

    expect(parsed.cards).toHaveLength(ROWS.length);
    // Cards come back sorted by id (per docs/deck-format.md), so match by front.
    const byFront = new Map(parsed.cards.map((c) => [c.front, c]));
    expect(byFront.get("爱")?.back).toBe("yêu; yêu thích");
    expect(byFront.get("爱")?.reading).toBe("ài");
    expect(byFront.get("chat")?.back).toBe("cat");

    // Syllable separator (U+001F) must survive the round trip untouched.
    const youshihou = byFront.get("有时候");
    expect(youshihou?.reading).toBe("yǒushíhou");

    // ids sorted ascending, matching the CARD section invariant.
    for (let i = 1; i < parsed.cards.length; i++) {
      expect(parsed.cards[i].id >= parsed.cards[i - 1].id).toBe(true);
    }
  });

  test("throws on bad magic", () => {
    const bad = new Uint8Array(32);
    bad.set(new TextEncoder().encode("NOTADECK"), 0);
    expect(() => parseDeck(bad)).toThrow(/bad magic/);
  });

  test("throws on a truncated buffer", () => {
    expect(() => parseDeck(new Uint8Array(10))).toThrow(/header/);
  });

  test("throws on a corrupted crc", async () => {
    const built = await buildDeck(ROWS, { name: "Test Deck", slug: "test-deck", lang: "zh" });
    const corrupted = new Uint8Array(built.bytes);
    // Flip a byte deep in the TEXT section, well past the header/section table.
    corrupted[corrupted.length - 1] ^= 0xff;
    expect(() => parseDeck(corrupted)).toThrow(/crc32 mismatch/);
  });

  test("skips an unknown section id rather than failing", async () => {
    const built = await buildDeck(ROWS, { name: "Test Deck", slug: "test-deck", lang: "zh" });
    const original = built.bytes;

    // Append one extra 16-byte section table entry with an unknown id
    // pointing at a zero-length blob at end-of-file, and bump
    // section_count + total_size. Inserting the entry after the existing
    // table (rather than before it) means every other section's recorded
    // offset stays valid, so only the header fields need updating. This
    // mirrors what a newer compiler might add (e.g. "FONT").
    const HEADER_SIZE = 32;
    const SECTION_SIZE = 16;
    const view = new DataView(original.buffer, original.byteOffset, original.byteLength);
    const oldSectionCount = view.getUint32(12, true);
    const oldTotalSize = view.getUint32(8, true);
    const tableEnd = HEADER_SIZE + oldSectionCount * SECTION_SIZE;

    const out = new Uint8Array(oldTotalSize + SECTION_SIZE);
    out.set(original.subarray(0, tableEnd), 0); // header + existing table (offsets fixed up below)
    out.set(original.subarray(tableEnd), tableEnd + SECTION_SIZE); // existing section bodies, shifted by +16

    const outView = new DataView(out.buffer);
    // The table grew by one entry, so every existing section's body moved
    // by SECTION_SIZE bytes; fix up their recorded offsets to match.
    for (let i = 0; i < oldSectionCount; i++) {
      const entryOff = HEADER_SIZE + i * SECTION_SIZE;
      const bodyOffset = outView.getUint32(entryOff + 4, true);
      outView.setUint32(entryOff + 4, bodyOffset + SECTION_SIZE, true);
    }

    const newEntryOffset = tableEnd; // where the extra table entry now lives
    out.set(new TextEncoder().encode("ZZZZ"), newEntryOffset);
    outView.setUint32(newEntryOffset + 4, oldTotalSize + SECTION_SIZE, true); // offset: end of file, 0 length
    outView.setUint32(newEntryOffset + 8, 0, true); // length
    outView.setUint32(newEntryOffset + 12, 0, true); // flags

    outView.setUint32(8, oldTotalSize + SECTION_SIZE, true); // total_size
    outView.setUint32(12, oldSectionCount + 1, true); // section_count

    // Recompute CRC over the new payload (everything after the header).
    const { crc32 } = await import("../lib/compiler/crc32");
    const payload = out.subarray(HEADER_SIZE, oldTotalSize + SECTION_SIZE);
    outView.setUint32(24, crc32(payload), true);

    const parsed = parseDeck(out);
    expect(parsed.cards).toHaveLength(ROWS.length);
    expect(parsed.meta.name).toBe("Test Deck");
  });
});
