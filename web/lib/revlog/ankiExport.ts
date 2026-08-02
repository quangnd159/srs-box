// CSV export of the revlog for import into desktop Anki, which recomputes
// its own FSRS schedule from these raw (card_id, timestamp, rating) rows
// rather than trusting a synced due date. See docs/deck-format.md and
// CLAUDE.md's "Anki remains authoritative for history".

import type { ReviewEntry } from "./parse";

export interface CardInfo {
  deckSlug: string;
  front: string;
}

const CSV_HEADER = "card_id,deck,front,reviewed_iso,rating,duration_ms";

function csvField(value: string): string {
  if (/[",\n]/.test(value)) {
    return `"${value.replace(/"/g, '""')}"`;
  }
  return value;
}

/**
 * Builds a CSV export of `entries`. Rows whose card_id has no entry in
 * `cards` still export, with empty deck/front columns, so an export never
 * silently drops history for an orphaned card id.
 */
export function toAnkiCsv(entries: readonly ReviewEntry[], cards: ReadonlyMap<bigint, CardInfo>): string {
  const lines = [CSV_HEADER];
  for (const e of entries) {
    const info = cards.get(e.cardId);
    const reviewedIso = new Date(Number(e.reviewed) * 1000).toISOString();
    lines.push(
      [
        csvField(e.cardId.toString()),
        csvField(info?.deckSlug ?? ""),
        csvField(info?.front ?? ""),
        csvField(reviewedIso),
        csvField(String(e.rating)),
        csvField(String(e.durationMs)),
      ].join(","),
    );
  }
  return lines.join("\n") + "\n";
}
