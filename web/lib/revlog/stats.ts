// Pure aggregation helpers over parsed revlog entries. Mirrors
// firmware/components/session/include/session.h's day_index(): a "day" rolls
// over at 4am local time, not midnight, so a late-night study session still
// counts toward the day it started. Default UTC offset is +7h, matching
// main.cpp (Vietnam).

import type { ReviewEntry } from "./parse";

export const DAY_ROLLOVER_HOUR = 4;
const SECONDS_PER_DAY = 86400n;
const DEFAULT_UTC_OFFSET_SECONDS = 7 * 3600;

/**
 * Local calendar day index for `unixSeconds`, rolling over at
 * DAY_ROLLOVER_HOUR local time. Mirrors session.h's day_index() exactly,
 * including floor (not truncating) division for timestamps before the
 * rollover epoch.
 */
export function dayIndex(unixSeconds: bigint, utcOffsetSeconds: number = DEFAULT_UTC_OFFSET_SECONDS): bigint {
  const local = unixSeconds + BigInt(utcOffsetSeconds) - BigInt(DAY_ROLLOVER_HOUR) * 3600n;
  if (local >= 0n) return local / SECONDS_PER_DAY;
  return -((-local + (SECONDS_PER_DAY - 1n)) / SECONDS_PER_DAY);
}

/** Converts a day index back to its "YYYY-MM-DD" label, in UTC, for display/keying. */
export function dayIndexToLabel(day: bigint): string {
  const ms = Number(day) * 86400 * 1000;
  return new Date(ms).toISOString().slice(0, 10);
}

/** Groups entries by local day (see dayIndex), sorted ascending by day. */
export function groupByDay(
  entries: readonly ReviewEntry[],
  utcOffsetSeconds: number = DEFAULT_UTC_OFFSET_SECONDS,
): Map<bigint, ReviewEntry[]> {
  const groups = new Map<bigint, ReviewEntry[]>();
  for (const e of entries) {
    const day = dayIndex(e.reviewed, utcOffsetSeconds);
    const bucket = groups.get(day);
    if (bucket) bucket.push(e);
    else groups.set(day, [e]);
  }
  return new Map([...groups.entries()].sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0)));
}

/** Review counts per day, keyed by the "YYYY-MM-DD" label, suitable for a heatmap. */
export function dailyCounts(
  entries: readonly ReviewEntry[],
  utcOffsetSeconds: number = DEFAULT_UTC_OFFSET_SECONDS,
): Map<string, number> {
  const counts = new Map<string, number>();
  for (const [day, group] of groupByDay(entries, utcOffsetSeconds)) {
    counts.set(dayIndexToLabel(day), group.length);
  }
  return counts;
}

export interface CardStats {
  cardId: bigint;
  reps: number;
  /** Count of rating === 1 ("Again") reviews after the card's first review. */
  lapses: number;
  lastReviewed: bigint;
}

/**
 * Per-card aggregation: total reps, lapses (Again-rated reviews after the
 * first review of that card), and the most recent review timestamp. Entries
 * are grouped by cardId and sorted by `reviewed` before counting lapses, so
 * result is order-independent regardless of input ordering.
 */
export function cardStats(entries: readonly ReviewEntry[]): Map<bigint, CardStats> {
  const byCard = new Map<bigint, ReviewEntry[]>();
  for (const e of entries) {
    const bucket = byCard.get(e.cardId);
    if (bucket) bucket.push(e);
    else byCard.set(e.cardId, [e]);
  }

  const result = new Map<bigint, CardStats>();
  for (const [cardId, group] of byCard) {
    const sorted = [...group].sort((a, b) => (a.reviewed < b.reviewed ? -1 : a.reviewed > b.reviewed ? 1 : 0));
    let lapses = 0;
    for (let i = 1; i < sorted.length; i++) {
      if (sorted[i].rating === 1) lapses++;
    }
    result.set(cardId, {
      cardId,
      reps: sorted.length,
      lapses,
      lastReviewed: sorted[sorted.length - 1].reviewed,
    });
  }
  return result;
}
