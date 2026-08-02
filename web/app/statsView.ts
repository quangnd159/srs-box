// Presentation-layer aggregation for /stats. lib/revlog/stats.ts owns the
// day-rollover arithmetic and per-card aggregation; this file only shapes
// those results into the rows and grids the page renders, and is separated
// out so all of that is testable without a DOM.

import type { CardInfo } from "@/lib/revlog/ankiExport";
import type { ReviewEntry } from "@/lib/revlog/parse";
import { cardStats, dailyCounts, dayIndex, dayIndexToLabel } from "@/lib/revlog/stats";

/** Matches lib/revlog/stats.ts's own default: Vietnam, as main.cpp uses. */
export const DEFAULT_UTC_OFFSET_SECONDS = 7 * 3600;

/** Deck slug used for reviews whose card is no longer in any known deck. */
export const UNKNOWN_DECK = "unknown";

export interface Totals {
  reviews: number;
  distinctCards: number;
  /** "YYYY-MM-DD" of the first review, or null when there are none. */
  firstDay: string | null;
  lastDay: string | null;
}

export function totals(
  entries: readonly ReviewEntry[],
  utcOffsetSeconds: number = DEFAULT_UTC_OFFSET_SECONDS,
): Totals {
  if (entries.length === 0) {
    return { reviews: 0, distinctCards: 0, firstDay: null, lastDay: null };
  }
  let min = entries[0].reviewed;
  let max = entries[0].reviewed;
  const cards = new Set<bigint>();
  for (const e of entries) {
    if (e.reviewed < min) min = e.reviewed;
    if (e.reviewed > max) max = e.reviewed;
    cards.add(e.cardId);
  }
  return {
    reviews: entries.length,
    distinctCards: cards.size,
    firstDay: dayIndexToLabel(dayIndex(min, utcOffsetSeconds)),
    lastDay: dayIndexToLabel(dayIndex(max, utcOffsetSeconds)),
  };
}

/**
 * Consecutive days ending today with at least one review. A gap for *today*
 * alone doesn't break the streak — the day isn't over yet — so a run that
 * ended yesterday still counts, exactly as Anki's streak does.
 */
export function currentStreak(
  entries: readonly ReviewEntry[],
  nowSeconds: bigint,
  utcOffsetSeconds: number = DEFAULT_UTC_OFFSET_SECONDS,
): number {
  const days = new Set<bigint>();
  for (const e of entries) days.add(dayIndex(e.reviewed, utcOffsetSeconds));
  if (days.size === 0) return 0;

  const today = dayIndex(nowSeconds, utcOffsetSeconds);
  let cursor = days.has(today) ? today : today - 1n;
  let streak = 0;
  while (days.has(cursor)) {
    streak++;
    cursor -= 1n;
  }
  return streak;
}

export interface DayCell {
  /** "YYYY-MM-DD", the key dailyCounts() uses. */
  label: string;
  count: number;
}

/**
 * The last `days` days, oldest first, each with its review count. Used both
 * by the 14-day bar list and (chunked into weeks) by the heatmap.
 */
export function lastDays(
  entries: readonly ReviewEntry[],
  nowSeconds: bigint,
  days: number,
  utcOffsetSeconds: number = DEFAULT_UTC_OFFSET_SECONDS,
): DayCell[] {
  const counts = dailyCounts(entries, utcOffsetSeconds);
  const today = dayIndex(nowSeconds, utcOffsetSeconds);
  const out: DayCell[] = [];
  for (let i = days - 1; i >= 0; i--) {
    const label = dayIndexToLabel(today - BigInt(i));
    out.push({ label, count: counts.get(label) ?? 0 });
  }
  return out;
}

/** Day of week for a day index, 0 = Sunday. Day index 0 is 1970-01-01, a Thursday. */
export function weekdayOf(day: bigint): number {
  return Number(((day % 7n) + 11n) % 7n); // +4 for Thursday, +7 to stay non-negative
}

/**
 * A GitHub-style grid: one column per week (Sunday-first), `weeks` columns
 * ending with the week containing today. Cells before the first day and after
 * today are null so the first and last columns are ragged, as they should be.
 */
export function heatmapWeeks(
  entries: readonly ReviewEntry[],
  nowSeconds: bigint,
  weeks = 26,
  utcOffsetSeconds: number = DEFAULT_UTC_OFFSET_SECONDS,
): (DayCell | null)[][] {
  const counts = dailyCounts(entries, utcOffsetSeconds);
  const today = dayIndex(nowSeconds, utcOffsetSeconds);
  const lastSunday = today - BigInt(weekdayOf(today));
  const firstSunday = lastSunday - BigInt((weeks - 1) * 7);

  const grid: (DayCell | null)[][] = [];
  for (let w = 0; w < weeks; w++) {
    const column: (DayCell | null)[] = [];
    for (let d = 0; d < 7; d++) {
      const day = firstSunday + BigInt(w * 7 + d);
      if (day > today) {
        column.push(null);
        continue;
      }
      const label = dayIndexToLabel(day);
      column.push({ label, count: counts.get(label) ?? 0 });
    }
    grid.push(column);
  }
  return grid;
}

/** Five buckets, matching GitHub's: 0, then quartiles of the busiest day. */
export function heatLevel(count: number, max: number): 0 | 1 | 2 | 3 | 4 {
  if (count <= 0) return 0;
  if (max <= 0) return 1;
  const ratio = count / max;
  if (ratio <= 0.25) return 1;
  if (ratio <= 0.5) return 2;
  if (ratio <= 0.75) return 3;
  return 4;
}

export interface DeckStatRow {
  deckSlug: string;
  reviews: number;
  distinctCards: number;
  lapses: number;
}

/**
 * Per-deck rollup. Cards missing from `cards` (their deck was deleted since
 * the pull) land in UNKNOWN_DECK rather than being dropped, so the totals in
 * this table always add up to the headline review count.
 */
export function perDeckStats(
  entries: readonly ReviewEntry[],
  cards: ReadonlyMap<bigint, CardInfo>,
): DeckStatRow[] {
  const perCard = cardStats(entries);
  const rows = new Map<string, DeckStatRow>();
  for (const stats of perCard.values()) {
    const slug = cards.get(stats.cardId)?.deckSlug || UNKNOWN_DECK;
    const row = rows.get(slug) ?? { deckSlug: slug, reviews: 0, distinctCards: 0, lapses: 0 };
    row.reviews += stats.reps;
    row.distinctCards += 1;
    row.lapses += stats.lapses;
    rows.set(slug, row);
  }
  return [...rows.values()].sort((a, b) => b.reviews - a.reviews);
}

/** Counts for ratings 1-4 (Again, Hard, Good, Easy), indexed 0-3. */
export function ratingCounts(entries: readonly ReviewEntry[]): [number, number, number, number] {
  const out: [number, number, number, number] = [0, 0, 0, 0];
  for (const e of entries) {
    if (e.rating >= 1 && e.rating <= 4) out[e.rating - 1]++;
  }
  return out;
}

/**
 * `cards` widened so every reviewed card id resolves. toAnkiCsv writes an
 * empty deck column for ids it can't find; filling them in as UNKNOWN_DECK
 * here keeps the export self-describing without touching lib/revlog.
 */
export function withUnknownDecks(
  entries: readonly ReviewEntry[],
  cards: ReadonlyMap<bigint, CardInfo>,
): Map<bigint, CardInfo> {
  const out = new Map(cards);
  for (const e of entries) {
    if (!out.has(e.cardId)) out.set(e.cardId, { deckSlug: UNKNOWN_DECK, front: "" });
  }
  return out;
}
