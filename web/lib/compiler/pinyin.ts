// Pinyin syllable segmentation, the TypeScript twin of tools/deckc.py's
// `segment_reading`. Both must agree exactly, since the acceptance test
// byte-compares their .srs output (web/test/deckc.acceptance.test.ts).
//
// The display reading ("yǒushíhou") carries tone marks but no syllable
// boundaries, and a syllable's coda sits *after* its tone mark, so the
// device cannot recover the boundaries by scanning for tone marks alone:
// "yǒushíhou" would come out as "yǒ|ushí|hou". The boundaries are however
// already in the source, in the numeric-pinyin column ("you3shi2hou5"), so
// the compiler does the split once and records it.
//
// The result is written into the reading field with U+001F (ASCII unit
// separator) between syllables. It is a control character, so it can never
// collide with reading text, never reaches a font subset, and old firmware
// that ignores it degrades to the old heuristic rather than mis-rendering.

export const SYLLABLE_SEP = "\u001f";

// Tone-marked vowels folded to their base letter, so a numeric syllable
// ("lve") can be compared against the display letters ("lüè"). ü and its
// toned forms fold to "v", matching the "v"/"u:" spellings the numeric
// column uses for it.
const FOLD: Record<string, string> = {};
for (const [base, marked] of [
  ["a", "āáǎà"],
  ["e", "ēéěè"],
  ["i", "īíǐì"],
  ["o", "ōóǒò"],
  ["u", "ūúǔù"],
  ["v", "ǖǘǚǜü"],
] as const) {
  for (const ch of marked) FOLD[ch] = base;
}

// Characters that are part of a reading but not part of a syllable: the
// comma-space between alternate readings ("cháng, zhǎng"), the syllable
// apostrophe ("Xī'ān"), hyphens and the middle dot.
const GLUE = " ,'-·";

function fold(ch: string): string {
  const c = ch.toLowerCase();
  return FOLD[c] ?? c;
}

function isDigit(ch: string): boolean {
  return ch >= "0" && ch <= "9";
}

function isAsciiAlpha(ch: string): boolean {
  const c = ch.toLowerCase();
  return c >= "a" && c <= "z";
}

/** Matches Python's str.isalpha() closely enough for pinyin: letters only. */
function isAlpha(ch: string): boolean {
  return /\p{L}/u.test(ch);
}

/**
 * Splits a numeric-pinyin string into folded, digit-free syllables:
 * "you3shi2hou5" -> ["you", "shi", "hou"]. Throws on anything it cannot
 * account for, because a guessed split is worse than no split.
 */
export function numericSyllables(numeric: string): string[] {
  const out: string[] = [];
  let cur = "";
  const chars = Array.from(numeric);
  for (let i = 0; i < chars.length; i++) {
    const ch = chars[i];
    if (isDigit(ch)) {
      if (!"12345".includes(ch) || !cur) throw new Error(`stray tone digit '${ch}'`);
      out.push(cur);
      cur = "";
    } else if (isAsciiAlpha(ch)) {
      let f = fold(ch);
      if (f === "u" && chars[i + 1] === ":") {
        f = "v"; // "lu:e4", the ASCII spelling of "lüe4"
        i++;
      }
      cur += f;
    } else if (ch === "ü" || ch === "Ü") {
      cur += "v";
    } else if (GLUE.includes(ch)) {
      // skip
    } else {
      throw new Error(`unexpected character '${ch}' in numeric pinyin`);
    }
  }
  if (cur) throw new Error(`syllable '${cur}' has no tone digit`);
  // Erhua: a trailing "r" is a suffix on the preceding syllable, not a
  // syllable of its own, so "hai2r5" is one run ("háir"), not two.
  const merged: string[] = [];
  for (const syl of out) {
    if (syl === "r" && merged.length > 0) merged[merged.length - 1] += "r";
    else merged.push(syl);
  }
  return merged;
}

/**
 * Returns `reading` with SYLLABLE_SEP inserted at syllable boundaries.
 *
 * Alignment is by folded letters: each numeric syllable consumes exactly
 * that many letters of the display reading, and glue characters (see GLUE)
 * ride along without being counted. Throws if the two disagree anywhere;
 * the caller must fail the compile rather than emit a guess.
 */
export function segmentReading(reading: string, numeric: string): string {
  if (!reading || !numeric) return reading;
  const syllables = numericSyllables(numeric);
  if (syllables.length === 0) throw new Error("no syllables in the numeric pinyin");

  const chars = Array.from(reading);
  const segments: string[] = [];
  let i = 0;
  for (const syl of syllables) {
    let seg = "";
    let j = 0;
    while (j < syl.length) {
      if (i >= chars.length) throw new Error(`reading ends before syllable '${syl}'`);
      const ch = chars[i];
      if (isAlpha(ch)) {
        if (fold(ch) !== syl[j]) throw new Error(`'${ch}' does not match '${syl[j]}' of '${syl}'`);
        j++;
      } else if (GLUE.includes(ch)) {
        if (j !== 0) throw new Error(`'${ch}' splits syllable '${syl}'`);
      } else {
        throw new Error(`unexpected character '${ch}' in the reading`);
      }
      seg += ch;
      i++;
    }
    segments.push(seg);
  }

  while (i < chars.length) {
    // Trailing glue joins the last syllable.
    const ch = chars[i];
    if (isAlpha(ch)) throw new Error(`unconsumed reading text '${chars.slice(i).join("")}'`);
    if (!GLUE.includes(ch)) throw new Error(`unexpected character '${ch}' in the reading`);
    segments[segments.length - 1] += ch;
    i++;
  }

  return segments.join(SYLLABLE_SEP);
}
