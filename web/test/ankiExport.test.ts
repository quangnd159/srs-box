import { describe, expect, test } from "bun:test";
import { toAnkiCsv, type CardInfo } from "../lib/revlog/ankiExport";
import type { ReviewEntry } from "../lib/revlog/parse";

describe("toAnkiCsv", () => {
  test("emits header plus one row per entry", () => {
    const entries: ReviewEntry[] = [
      { cardId: 1n, reviewed: 0n, rating: 3, stateBefore: 0, durationMs: 1200 },
    ];
    const cards = new Map<bigint, CardInfo>([[1n, { deckSlug: "hsk1", front: "爱" }]]);

    const csv = toAnkiCsv(entries, cards);
    const lines = csv.trim().split("\n");
    expect(lines[0]).toBe("card_id,deck,front,reviewed_iso,rating,duration_ms");
    expect(lines[1]).toBe("1,hsk1,爱,1970-01-01T00:00:00.000Z,3,1200");
  });

  test("quotes fields containing commas, quotes, or newlines", () => {
    const entries: ReviewEntry[] = [
      { cardId: 2n, reviewed: 0n, rating: 4, stateBefore: 2, durationMs: 500 },
    ];
    const cards = new Map<bigint, CardInfo>([
      [2n, { deckSlug: "generic", front: 'say "hi", bye\nnext line' }],
    ]);

    const csv = toAnkiCsv(entries, cards);
    const lines = csv.trim().split("\n");
    expect(lines.slice(1).join("\n")).toContain('"say ""hi"", bye\nnext line"');
  });

  test("missing card info exports with empty deck/front rather than dropping the row", () => {
    const entries: ReviewEntry[] = [
      { cardId: 99n, reviewed: 0n, rating: 2, stateBefore: 1, durationMs: 900 },
    ];
    const csv = toAnkiCsv(entries, new Map());
    const lines = csv.trim().split("\n");
    expect(lines[1]).toBe("99,,,1970-01-01T00:00:00.000Z,2,900");
  });
});
