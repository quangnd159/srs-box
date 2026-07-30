import { describe, expect, test } from "bun:test";
import { toDiacritics, toneSequence } from "../lib/cedict/pinyin";

describe("toDiacritics", () => {
  test("marks a-class vowels first", () => {
    expect(toDiacritics("hao3")).toBe("hǎo");
    expect(toDiacritics("bao3")).toBe("bǎo");
  });

  test("marks e over other vowels", () => {
    expect(toDiacritics("bei3")).toBe("běi");
  });

  test("marks o in ou diphthongs", () => {
    expect(toDiacritics("dou1")).toBe("dōu");
  });

  test("marks the last vowel otherwise (e.g. -iu, -ui)", () => {
    expect(toDiacritics("liu2")).toBe("liú");
    expect(toDiacritics("gui4")).toBe("guì");
  });

  test("leaves neutral tone (5) unmarked", () => {
    expect(toDiacritics("ma5")).toBe("ma");
  });

  test("handles ü spelled as u:", () => {
    expect(toDiacritics("nu:3")).toBe("nǚ");
  });

  test("joins multiple syllables with a space", () => {
    expect(toDiacritics("ni3 hao3")).toBe("nǐ hǎo");
  });

  test("preserves capitalization for proper nouns", () => {
    expect(toDiacritics("Bei3 jing1")).toBe("Běi jīng");
  });
});

describe("toneSequence", () => {
  test("extracts tone numbers per syllable", () => {
    expect(toneSequence("ni3 hao3 ma5")).toEqual([3, 3, 5]);
  });
});
