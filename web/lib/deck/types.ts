/** A deck's identity and metadata, persisted separately from its cards. */
export interface Deck {
  /** Stable slug: the compiler's deck id input, and the device's file/path
   * key (decks/<slug>.srs). Changing it orphans all existing card history;
   * see docs/deck-format.md. */
  slug: string;
  /** Display name shown on the device's deck-picker screen. */
  name: string;
  /** BCP47-ish tag: "zh" enables pinyin tone-colouring, others render the
   * reading line in a neutral colour. See docs/sync-protocol.md. */
  lang: string;
  createdAt: string;
  updatedAt: string;
}

/** One row of a deck: headword, reading (pinyin/IPA/...), and gloss. */
export interface Card {
  /** Local-only identity for React keys and editing; not the device card id
   * (that's derived at compile time from slug + front, see stableId.ts). */
  localId: string;
  front: string;
  reading: string;
  back: string;
  /** Numeric-tone pinyin for `reading`, when known (from a 5-column paste or
   * CC-CEDICT autofill). The compiler uses it to split the reading into
   * syllables; it is cleared whenever the reading is edited by hand, since a
   * stale one would no longer describe it. See lib/compiler/pinyin.ts. */
  numeric?: string;
}

export const SLUG_PATTERN = /^[a-z0-9][a-z0-9-]*$/;

export function isValidSlug(slug: string): boolean {
  return SLUG_PATTERN.test(slug);
}
