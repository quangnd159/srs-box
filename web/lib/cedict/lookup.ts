// Client-side CC-CEDICT lookup, loaded lazily (only fetched when a zh-lang
// deck actually needs autofill) from public/data/cedict.compact.json, which
// scripts/fetch-cedict.ts compiles from the CC-BY-SA CC-CEDICT source; see
// data/CEDICT-LICENSE.txt.
import { toDiacritics } from "./pinyin";

export interface CedictEntry {
  pinyin: string; // numeric tone, e.g. "ni3 hao3"
  gloss: string;
}

export type CedictTable = Record<string, CedictEntry[]>;

let cached: Promise<CedictTable> | null = null;

/** Fetches (and memoizes) the compiled CC-CEDICT table. Browser-only. */
export function loadCedict(): Promise<CedictTable> {
  if (!cached) {
    cached = fetch("/data/cedict.compact.json")
      .then((res) => {
        if (!res.ok) throw new Error(`failed to load CC-CEDICT: ${res.status}`);
        return res.json() as Promise<CedictTable>;
      })
      .catch((err) => {
        cached = null; // allow retry on next call
        throw err;
      });
  }
  return cached;
}

export interface AutofillResult {
  reading: string; // diacritic pinyin, ready to display/edit
  gloss: string;
}

/** Looks up a headword's first CC-CEDICT entry, converting pinyin to diacritics. */
export function autofill(table: CedictTable, headword: string): AutofillResult | undefined {
  const entries = table[headword];
  if (!entries || entries.length === 0) return undefined;
  const [first] = entries;
  return { reading: toDiacritics(first.pinyin), gloss: first.gloss };
}
