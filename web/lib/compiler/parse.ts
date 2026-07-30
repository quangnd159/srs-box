// TSV parser mirroring tools/deckc.py's read_rows(): tab-separated input in
// one of two shapes, auto-detected from the first data row's column count
// (a single file is entirely one shape or the other):
//
//   simplified \t traditional \t numeric-pinyin \t pinyin \t gloss   (5 columns, hskhsk.com)
//   front \t reading \t back                                        (3 columns, generic)
//
// Both formats dedup by headword (the first column) within a file, since a
// duplicate headword would collide on card id; the second occurrence is
// dropped, matching the Python compiler.

import type { DeckRow } from "./types";

function stripBom(text: string): string {
  return text.charCodeAt(0) === 0xfeff ? text.slice(1) : text;
}

function splitLines(text: string): string[] {
  // Matches Python's `line.rstrip("\n").rstrip("\r")` per line when
  // iterating a text-mode file: split on \n, then drop a trailing \r.
  return stripBom(text)
    .split("\n")
    .map((line) => (line.endsWith("\r") ? line.slice(0, -1) : line));
}

export interface ParseWarning {
  line: number;
  message: string;
}

export interface ParseResult {
  rows: DeckRow[];
  warnings: ParseWarning[];
}

/**
 * Parses tab-separated input, auto-detecting 5-column (hskhsk.com) vs.
 * 3-column (generic front/reading/back) from the first non-blank line,
 * exactly as tools/deckc.py's read_rows() does.
 */
export function parseTsv(text: string): ParseResult {
  const rows: DeckRow[] = [];
  const warnings: ParseWarning[] = [];
  const seen = new Set<string>();
  let expectedCols: 3 | 5 | null = null;
  const lines = splitLines(text);

  for (let i = 0; i < lines.length; i++) {
    const lineno = i + 1;
    const line = lines[i];
    if (!line.trim()) continue;
    const parts = line.split("\t");

    if (expectedCols === null) {
      expectedCols = parts.length < 5 ? 3 : 5;
    }

    if (parts.length < expectedCols) {
      warnings.push({
        line: lineno,
        message: `expected ${expectedCols} fields, got ${parts.length}`,
      });
      continue;
    }

    let front: string;
    let reading: string;
    let back: string;
    if (expectedCols === 5) {
      [front, , , reading, back] = parts;
    } else {
      [front, reading, back] = parts;
    }

    front = front.trim();
    if (!front) continue;
    if (seen.has(front)) {
      warnings.push({ line: lineno, message: `duplicate headword ${front}` });
      continue;
    }
    seen.add(front);
    rows.push({ front, reading: reading.trim(), back: back.trim() });
  }
  return { rows, warnings };
}

/** Convenience alias for callers that already know the input is 5-column hskhsk. */
export function parseHskRows(text: string): ParseResult {
  return parseTsv(text);
}

/** Convenience alias for callers that already know the input is 3-column generic. */
export function parseGenericRows(text: string): ParseResult {
  return parseTsv(text);
}
