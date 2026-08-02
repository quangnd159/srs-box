/** One normalized input row, prior to id assignment and pooling. */
export interface DeckRow {
  front: string;
  reading: string;
  back: string;
  /** Numeric-tone pinyin ("you3shi2hou5"), the authority on where this
   * reading's syllables end. Present for 5-column Chinese input; absent
   * elsewhere, in which case the reading ships unsegmented. See
   * lib/compiler/pinyin.ts. */
  numeric?: string;
}

/** Options for compiling a deck. `lang` is written to META when present. */
export interface BuildOptions {
  name: string;
  slug: string;
  lang?: string;
}

export interface BuildResult {
  bytes: Uint8Array;
  cardCount: number;
  textPoolBytes: number;
  uniqueStrings: number;
  glyphs: string[];
  cjkGlyphs: string[];
}
