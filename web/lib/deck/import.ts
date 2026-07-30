// Turns pasted text into Card rows for the deck editor: either a bare word
// list (one headword per line, to be autofilled) or a TSV in either format
// docs/sync-protocol.md describes (5-column hskhsk, or generic 3-column
// front/reading/back).
import { parseTsv } from "../compiler/parse";
import type { Card } from "./types";

function newLocalId(): string {
  return crypto.randomUUID();
}

/** One headword per line; reading and gloss are left blank for later autofill. */
export function importWordList(text: string): Card[] {
  return text
    .split("\n")
    .map((line) => line.trim())
    .filter(Boolean)
    .map((front) => ({ localId: newLocalId(), front, reading: "", back: "" }));
}

/** Detects and parses a TSV paste: 5-column hskhsk or generic 3-column front/reading/back. */
export function importTsv(text: string): { cards: Card[]; warnings: string[] } {
  const { rows, warnings } = parseTsv(text);
  return {
    cards: rows.map((r) => ({ localId: newLocalId(), ...r })),
    warnings: warnings.map((w) => `line ${w.line}: ${w.message}`),
  };
}
