// The bridge between /device (which pulls revlog.bin off the hardware) and
// /stats (which renders it with no device attached). localStorage, like the
// deck store, because this is a single-user tool.
//
// Card ids and review timestamps are bigint, and JSON.stringify throws on
// those, so everything wide is stored as a decimal string and widened back on
// read. The stored card map is what makes a deck-attributed export possible
// after a deck has been deleted from the device.

import type { CardInfo } from "@/lib/revlog/ankiExport";
import type { ReviewEntry } from "@/lib/revlog/parse";
import type { KeyValueStore } from "@/lib/deck/store";

export const REVLOG_KEY = "srsbox.revlog.latest";

/** On-disk shape. Keys are short because a long revlog is a lot of rows. */
interface StoredEntry {
  c: string; // cardId
  t: string; // reviewed, unix seconds
  r: number; // rating
  s: number; // stateBefore
  d: number; // durationMs
}

interface StoredRevlog {
  v: 1;
  pulledAt: string;
  version: number;
  entries: StoredEntry[];
  cards: Record<string, CardInfo>;
}

export interface RevlogSnapshot {
  /** ISO timestamp of the pull that produced this snapshot. */
  pulledAt: string;
  /** revlog.bin format version, as reported by the file header. */
  version: number;
  entries: ReviewEntry[];
  cards: Map<bigint, CardInfo>;
}

export function saveRevlog(kv: KeyValueStore, snapshot: RevlogSnapshot): void {
  const stored: StoredRevlog = {
    v: 1,
    pulledAt: snapshot.pulledAt,
    version: snapshot.version,
    entries: snapshot.entries.map((e) => ({
      c: e.cardId.toString(),
      t: e.reviewed.toString(),
      r: e.rating,
      s: e.stateBefore,
      d: e.durationMs,
    })),
    cards: Object.fromEntries([...snapshot.cards].map(([id, info]) => [id.toString(), info])),
  };
  kv.setItem(REVLOG_KEY, JSON.stringify(stored));
}

/** Returns null when nothing has been pulled yet, or when the blob is unreadable. */
export function loadRevlog(kv: KeyValueStore): RevlogSnapshot | null {
  const raw = kv.getItem(REVLOG_KEY);
  if (!raw) return null;
  let stored: StoredRevlog;
  try {
    stored = JSON.parse(raw) as StoredRevlog;
  } catch {
    return null;
  }
  if (!stored || !Array.isArray(stored.entries)) return null;

  return {
    pulledAt: stored.pulledAt ?? "",
    version: stored.version ?? 0,
    entries: stored.entries.map((e) => ({
      cardId: BigInt(e.c),
      reviewed: BigInt(e.t),
      rating: e.r,
      stateBefore: e.s,
      durationMs: e.d,
    })),
    cards: new Map(
      Object.entries(stored.cards ?? {}).map(([id, info]) => [BigInt(id), info]),
    ),
  };
}

export function clearRevlog(kv: KeyValueStore): void {
  kv.removeItem(REVLOG_KEY);
}
