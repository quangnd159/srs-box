// Converts CC-CEDICT's numeric-tone pinyin ("ni3 hao3") to the diacritic
// form used for display ("nǐ hǎo"), and to a tone number (1-5) per syllable
// used for tone-colouring the reading line (see docs/deck-format.md: "lang
// gates language-specific rendering: pinyin tone-colouring applies only
// when lang=zh").

// Indexed by tone - 1, for tones 1-4. Tone 5 (neutral) and tone 0
// (unrecognized) leave the vowel unmarked.
const TONE_MARKS: Record<string, string[]> = {
  a: ["ā", "á", "ǎ", "à"],
  e: ["ē", "é", "ě", "è"],
  i: ["ī", "í", "ǐ", "ì"],
  o: ["ō", "ó", "ǒ", "ò"],
  u: ["ū", "ú", "ǔ", "ù"],
  "ü": ["ǖ", "ǘ", "ǚ", "ǜ"],
};

/** Marks the vowel that carries the tone, per the standard pinyin rule. */
function markSyllable(letters: string, tone: number): string {
  const lower = letters.toLowerCase();
  let vowelIndex = -1;

  if (lower.includes("a")) vowelIndex = lower.indexOf("a");
  else if (lower.includes("e")) vowelIndex = lower.indexOf("e");
  else if (lower.includes("ou")) vowelIndex = lower.indexOf("o");
  else {
    // Last vowel in the syllable (covers -iu, -ui diphthongs).
    for (let i = lower.length - 1; i >= 0; i--) {
      if ("aeiou".includes(lower[i]) || lower[i] === "ü") {
        vowelIndex = i;
        break;
      }
    }
  }
  if (vowelIndex === -1 || tone < 1 || tone > 4) return letters;

  const vowel = lower[vowelIndex];
  const marked = TONE_MARKS[vowel]?.[tone - 1];
  if (!marked) return letters;

  const isUpper = letters[vowelIndex] === letters[vowelIndex].toUpperCase();
  const replacement = isUpper ? marked.toUpperCase() : marked;
  return letters.slice(0, vowelIndex) + replacement + letters.slice(vowelIndex + 1);
}

/** One syllable: numeric form ("ni3") to diacritic form ("nǐ") plus its tone. */
export function syllableToDiacritic(syllable: string): { text: string; tone: number } {
  // CC-CEDICT spells ü as "u:" (and sometimes bare "v").
  const normalized = syllable.replace(/u:/gi, "ü").replace(/v/gi, "ü");
  const match = normalized.match(/^([a-zA-Züêr]+)([0-5])$/i);
  if (!match) return { text: normalized, tone: 0 };
  const [, letters, toneStr] = match;
  const tone = Number(toneStr);
  return { text: markSyllable(letters, tone), tone };
}

/** Converts a full numeric-tone pinyin string ("ni3 hao3") to diacritics ("nǐ hǎo"). */
export function toDiacritics(numericPinyin: string): string {
  return numericPinyin
    .split(/\s+/)
    .filter(Boolean)
    .map((syl) => syllableToDiacritic(syl).text)
    .join(" ");
}

/** Per-syllable tones (1-5, 0 = unrecognized) for tone-colouring the reading line. */
export function toneSequence(numericPinyin: string): number[] {
  return numericPinyin
    .split(/\s+/)
    .filter(Boolean)
    .map((syl) => syllableToDiacritic(syl).tone);
}
