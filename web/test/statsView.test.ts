import { describe, expect, test } from "bun:test";
import {
  currentStreak,
  heatLevel,
  heatmapWeeks,
  lastDays,
  perDeckStats,
  ratingCounts,
  totals,
  UNKNOWN_DECK,
  weekdayOf,
  withUnknownDecks,
} from "../app/statsView";
import type { CardInfo } from "../lib/revlog/ankiExport";
import { toAnkiCsv } from "../lib/revlog/ankiExport";
import type { ReviewEntry } from "../lib/revlog/parse";
import { dayIndex } from "../lib/revlog/stats";

// A timestamp comfortably inside local day `d` (offset +7h, 4am rollover, so
// local noon is well clear of both boundaries).
function atDay(d: bigint): bigint {
  return d * 86400n - 10800n + 43200n;
}

function review(day: bigint, cardId: number, rating = 3): ReviewEntry {
  return {
    cardId: BigInt(cardId),
    reviewed: atDay(day),
    rating,
    stateBefore: 2,
    durationMs: 1500,
  };
}

const TODAY = dayIndex(BigInt(Date.UTC(2026, 6, 30, 12, 0, 0) / 1000));
const NOW = atDay(TODAY);

describe("atDay fixture", () => {
  test("lands inside the day it names", () => {
    expect(dayIndex(atDay(TODAY))).toBe(TODAY);
    expect(dayIndex(atDay(TODAY - 5n))).toBe(TODAY - 5n);
  });
});

describe("totals", () => {
  test("counts reviews, distinct cards, and the first/last day", () => {
    const t = totals([review(TODAY - 3n, 1), review(TODAY, 1), review(TODAY, 2)]);
    expect(t.reviews).toBe(3);
    expect(t.distinctCards).toBe(2);
    expect(t.firstDay).toBe("2026-07-27");
    expect(t.lastDay).toBe("2026-07-30");
  });

  test("is empty-safe", () => {
    expect(totals([])).toEqual({ reviews: 0, distinctCards: 0, firstDay: null, lastDay: null });
  });
});

describe("currentStreak", () => {
  test("counts consecutive days ending today", () => {
    const entries = [review(TODAY, 1), review(TODAY - 1n, 1), review(TODAY - 2n, 1)];
    expect(currentStreak(entries, NOW)).toBe(3);
  });

  test("survives a day with nothing studied yet today", () => {
    const entries = [review(TODAY - 1n, 1), review(TODAY - 2n, 1)];
    expect(currentStreak(entries, NOW)).toBe(2);
  });

  test("breaks on a missing day", () => {
    const entries = [review(TODAY, 1), review(TODAY - 2n, 1), review(TODAY - 3n, 1)];
    expect(currentStreak(entries, NOW)).toBe(1);
  });

  test("is 0 when the last review is older than yesterday", () => {
    expect(currentStreak([review(TODAY - 5n, 1)], NOW)).toBe(0);
    expect(currentStreak([], NOW)).toBe(0);
  });
});

describe("lastDays", () => {
  test("returns exactly N days, oldest first, zero-filling gaps", () => {
    const days = lastDays([review(TODAY, 1), review(TODAY, 2), review(TODAY - 13n, 3)], NOW, 14);
    expect(days).toHaveLength(14);
    expect(days[0].count).toBe(1);
    expect(days[13].count).toBe(2);
    expect(days[13].label).toBe("2026-07-30");
    expect(days.slice(1, 13).every((d) => d.count === 0)).toBe(true);
  });
});

describe("weekdayOf", () => {
  test("day 0 is a Thursday and the week wraps", () => {
    expect(weekdayOf(0n)).toBe(4);
    expect(weekdayOf(3n)).toBe(0); // 1970-01-04, a Sunday
    expect(weekdayOf(-1n)).toBe(3);
    expect(weekdayOf(-100n)).toBe(2);
  });
});

describe("heatmapWeeks", () => {
  test("is a 26-column, 7-row grid ending in today's week", () => {
    const grid = heatmapWeeks([review(TODAY, 1)], NOW, 26);
    expect(grid).toHaveLength(26);
    expect(grid.every((week) => week.length === 7)).toBe(true);

    const last = grid[25];
    expect(last[weekdayOf(TODAY)]?.count).toBe(1);
    // Future days in the current week are blanks, not zero cells.
    for (let d = weekdayOf(TODAY) + 1; d < 7; d++) expect(last[d]).toBeNull();
  });

  test("each column runs Sunday to Saturday", () => {
    const grid = heatmapWeeks([], NOW, 4);
    const first = grid[0][0];
    expect(first).not.toBeNull();
    expect(new Date(first!.label + "T00:00:00Z").getUTCDay()).toBe(0);
  });
});

describe("heatLevel", () => {
  test("0 reviews is level 0 and the busiest day is level 4", () => {
    expect(heatLevel(0, 10)).toBe(0);
    expect(heatLevel(10, 10)).toBe(4);
    expect(heatLevel(1, 10)).toBe(1);
    expect(heatLevel(4, 10)).toBe(2);
    expect(heatLevel(7, 10)).toBe(3);
    expect(heatLevel(1, 0)).toBe(1);
  });
});

describe("perDeckStats", () => {
  const cards = new Map<bigint, CardInfo>([
    [1n, { deckSlug: "hsk1", front: "我" }],
    [2n, { deckSlug: "hsk1", front: "你" }],
    [3n, { deckSlug: "fr-a1", front: "bonjour" }],
  ]);

  test("rolls up reviews, cards, and lapses per deck", () => {
    const entries = [
      review(TODAY - 2n, 1),
      review(TODAY - 1n, 1, 1), // lapse: an Again after the first review
      review(TODAY, 2),
      review(TODAY, 3),
    ];
    const rows = perDeckStats(entries, cards);
    expect(rows[0]).toEqual({ deckSlug: "hsk1", reviews: 3, distinctCards: 2, lapses: 1 });
    expect(rows[1]).toEqual({ deckSlug: "fr-a1", reviews: 1, distinctCards: 1, lapses: 0 });
  });

  test("cards with no known deck land in 'unknown' rather than vanishing", () => {
    const rows = perDeckStats([review(TODAY, 99)], cards);
    expect(rows).toEqual([{ deckSlug: UNKNOWN_DECK, reviews: 1, distinctCards: 1, lapses: 0 }]);
  });
});

describe("ratingCounts", () => {
  test("buckets ratings 1-4 and ignores anything else", () => {
    const entries = [
      review(TODAY, 1, 1),
      review(TODAY, 2, 3),
      review(TODAY, 3, 3),
      review(TODAY, 4, 4),
      review(TODAY, 5, 9),
    ];
    expect(ratingCounts(entries)).toEqual([1, 0, 2, 1]);
  });
});

describe("withUnknownDecks", () => {
  test("fills in missing ids so toAnkiCsv exports them with a deck", () => {
    const cards = new Map<bigint, CardInfo>([[1n, { deckSlug: "hsk1", front: "我" }]]);
    const entries = [review(TODAY, 1), review(TODAY, 2)];
    const csv = toAnkiCsv(entries, withUnknownDecks(entries, cards));
    const lines = csv.trim().split("\n");
    expect(lines).toHaveLength(3);
    expect(lines[1]).toContain(",hsk1,");
    expect(lines[2]).toContain(`,${UNKNOWN_DECK},`);
  });

  test("leaves the input map untouched", () => {
    const cards = new Map<bigint, CardInfo>([[1n, { deckSlug: "hsk1", front: "我" }]]);
    withUnknownDecks([review(TODAY, 2)], cards);
    expect(cards.size).toBe(1);
  });
});
