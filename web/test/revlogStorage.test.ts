import { describe, expect, test } from "bun:test";
import { clearRevlog, loadRevlog, REVLOG_KEY, saveRevlog } from "../app/revlogStorage";
import { MemoryStore } from "../lib/deck/store";
import type { CardInfo } from "../lib/revlog/ankiExport";
import type { ReviewEntry } from "../lib/revlog/parse";

const entries: ReviewEntry[] = [
  // Deliberately past 2^53: card ids are 64-bit, so this is exactly the case
  // a naive Number() round trip would corrupt.
  { cardId: 0xfedcba9876543210n, reviewed: 1785000000n, rating: 3, stateBefore: 2, durationMs: 900 },
  { cardId: 1n, reviewed: 1785086400n, rating: 1, stateBefore: 0, durationMs: 4200 },
];

const cards = new Map<bigint, CardInfo>([
  [0xfedcba9876543210n, { deckSlug: "hsk1", front: "我" }],
  [1n, { deckSlug: "fr-a1", front: "bonjour" }],
]);

describe("revlog localStorage snapshot", () => {
  test("round-trips bigint ids and timestamps without loss", () => {
    const kv = new MemoryStore();
    saveRevlog(kv, { pulledAt: "2026-07-30T09:00:00.000Z", version: 1, entries, cards });

    const loaded = loadRevlog(kv);
    expect(loaded).not.toBeNull();
    expect(loaded!.version).toBe(1);
    expect(loaded!.pulledAt).toBe("2026-07-30T09:00:00.000Z");
    expect(loaded!.entries).toEqual(entries);
    expect(loaded!.cards.get(0xfedcba9876543210n)).toEqual({ deckSlug: "hsk1", front: "我" });
  });

  test("stores wide values as strings, so JSON.stringify never sees a bigint", () => {
    const kv = new MemoryStore();
    saveRevlog(kv, { pulledAt: "x", version: 1, entries, cards });
    const raw = kv.getItem(REVLOG_KEY)!;
    expect(raw).toContain("18364758544493064720"); // 0xfedcba9876543210 in decimal
    expect(() => JSON.parse(raw)).not.toThrow();
  });

  test("returns null when nothing has been pulled", () => {
    expect(loadRevlog(new MemoryStore())).toBeNull();
  });

  test("returns null rather than throwing on a corrupt blob", () => {
    const kv = new MemoryStore();
    kv.setItem(REVLOG_KEY, "{not json");
    expect(loadRevlog(kv)).toBeNull();
    kv.setItem(REVLOG_KEY, '{"v":1,"entries":"nope"}');
    expect(loadRevlog(kv)).toBeNull();
  });

  test("clear removes the snapshot", () => {
    const kv = new MemoryStore();
    saveRevlog(kv, { pulledAt: "x", version: 1, entries, cards });
    clearRevlog(kv);
    expect(loadRevlog(kv)).toBeNull();
  });

  test("an empty card map is preserved (uploaded revlog with no deck info)", () => {
    const kv = new MemoryStore();
    saveRevlog(kv, { pulledAt: "x", version: 1, entries, cards: new Map() });
    expect(loadRevlog(kv)!.cards.size).toBe(0);
  });
});
