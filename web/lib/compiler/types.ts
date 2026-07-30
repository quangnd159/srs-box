/** One normalized input row, prior to id assignment and pooling. */
export interface DeckRow {
  front: string;
  reading: string;
  back: string;
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
