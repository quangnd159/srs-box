import { describe, expect, test } from "bun:test";
import { cardStats, dailyCounts, dayIndex, groupByDay } from "../lib/revlog/stats";
import type { ReviewEntry } from "../lib/revlog/parse";

function entry(cardId: bigint, reviewed: bigint, rating: number, durationMs = 1000): ReviewEntry {
  return { cardId, reviewed, rating, stateBefore: 2, durationMs };
}

describe("dayIndex: 4am local rollover", () => {
  // Use UTC offset 0 so "local" and "unix seconds" line up directly.
  const DAY_SECONDS = 86400n;

  test("03:59:59 belongs to the previous day", () => {
    const justBeforeRollover = DAY_SECONDS + 4n * 3600n - 1n; // 1970-01-02T03:59:59Z
    expect(dayIndex(justBeforeRollover, 0)).toBe(0n);
  });

  test("04:00:00 belongs to the new day", () => {
    const atRollover = DAY_SECONDS + 4n * 3600n; // 1970-01-02T04:00:00Z
    expect(dayIndex(atRollover, 0)).toBe(1n);
  });

  test("handles timestamps before the epoch with floor division", () => {
    // 1969-12-31T03:00:00Z: one hour before that day's rollover, so it
    // should fall in the day before (index -2, i.e. Dec 30).
    const beforeEpoch = -DAY_SECONDS + 3n * 3600n;
    expect(dayIndex(beforeEpoch, 0)).toBe(-2n);
  });

  test("a positive UTC offset shifts the rollover earlier in UTC terms", () => {
    // UTC+7: local 04:00:00 is UTC 21:00:00 the previous day.
    const utcOffset = 7 * 3600;
    const localRollover = DAY_SECONDS + 4n * 3600n - BigInt(utcOffset); // that UTC instant
    expect(dayIndex(localRollover, utcOffset)).toBe(1n);
    expect(dayIndex(localRollover - 1n, utcOffset)).toBe(0n);
  });
});

describe("groupByDay / dailyCounts", () => {
  test("groups reviews spanning a rollover into the correct days", () => {
    const entries = [
      entry(1n, 3n * 3600n, 3), // 03:00 UTC, before the 04:00 rollover -> previous day
      entry(2n, 5n * 3600n, 3), // 05:00 UTC, after rollover -> next day
      entry(3n, 86400n + 5n * 3600n, 3), // 24h later, 05:00 -> day after that
    ];
    const grouped = groupByDay(entries, 0);
    const days = [...grouped.keys()];
    expect(days).toHaveLength(3);
    // Consecutive, ascending, and the 03:00 entry lands a day before the 05:00 one.
    expect(days[1]).toBe(days[0] + 1n);
    expect(days[2]).toBe(days[1] + 1n);
    expect(grouped.get(days[0])).toHaveLength(1);

    const counts = dailyCounts(entries, 0);
    expect(counts.size).toBe(3);
    expect([...counts.values()]).toEqual([1, 1, 1]);
  });
});

describe("cardStats", () => {
  test("counts reps, lapses after the first review, and last reviewed", () => {
    const entries = [
      entry(1n, 100n, 3),
      entry(1n, 200n, 1), // lapse
      entry(1n, 50n, 1), // earliest review; rating 1 here does NOT count as a lapse
      entry(1n, 300n, 4),
      entry(2n, 10n, 3),
    ];
    const stats = cardStats(entries);
    const card1 = stats.get(1n)!;
    expect(card1.reps).toBe(4);
    expect(card1.lapses).toBe(1); // only the rating=1 at t=200, not the first review at t=50
    expect(card1.lastReviewed).toBe(300n);

    const card2 = stats.get(2n)!;
    expect(card2.reps).toBe(1);
    expect(card2.lapses).toBe(0);
    expect(card2.lastReviewed).toBe(10n);
  });
});
