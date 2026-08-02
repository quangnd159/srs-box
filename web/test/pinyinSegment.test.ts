// Syllable segmentation of the display reading, the half of the compiler
// that the device cannot do for itself (lib/compiler/pinyin.ts). The Python
// twin's equivalent cases live in tools/test_deckc.py, and the byte-identity
// of the two is covered by deckc.acceptance.test.ts.
import { describe, expect, test } from "bun:test";

import { buildDeck } from "../lib/compiler/build";
import { SYLLABLE_SEP, numericSyllables, segmentReading } from "../lib/compiler/pinyin";

const SEP = SYLLABLE_SEP;

describe("numericSyllables", () => {
  test("splits on tone digits", () => {
    expect(numericSyllables("you3shi2hou5")).toEqual(["you", "shi", "hou"]);
  });

  test("folds ü, u: and v together", () => {
    expect(numericSyllables("lü3you2")).toEqual(["lv", "you"]);
    expect(numericSyllables("lu:3you2")).toEqual(["lv", "you"]);
    expect(numericSyllables("lv3you2")).toEqual(["lv", "you"]);
  });

  test("treats erhua as a suffix, not a syllable", () => {
    expect(numericSyllables("nü3hai2r5")).toEqual(["nv", "hair"]);
  });

  test("rejects letters with no tone digit", () => {
    expect(() => numericSyllables("zhen1de")).toThrow("no tone digit");
  });
});

describe("segmentReading", () => {
  test("keeps a syllable's coda with its own syllable", () => {
    // The bug this whole mechanism exists for: scanning tone marks alone
    // gives "yǒ|ushí|hou".
    expect(segmentReading("yǒushíhou", "you3shi2hou5")).toBe(`yǒu${SEP}shí${SEP}hou`);
  });

  test("splits a neutral-tone syllable off rather than absorbing it", () => {
    expect(segmentReading("bàba", "ba4ba5")).toBe(`bà${SEP}ba`);
  });

  test("aligns ü in the reading against ü, u: or v in the numeric column", () => {
    expect(segmentReading("lǚyóu", "lü3you2")).toBe(`lǚ${SEP}yóu`);
    expect(segmentReading("hūlüè", "hu1lu:e4")).toBe(`hū${SEP}lüè`);
    expect(segmentReading("nǚháir", "nü3hai2r5")).toBe(`nǚ${SEP}háir`);
  });

  test("carries punctuation between alternate readings without counting it", () => {
    expect(segmentReading("cháng, zhǎng", "chang2, zhang3")).toBe(`cháng${SEP}, zhǎng`);
  });

  test("passes an unsegmentable input straight through when either side is empty", () => {
    expect(segmentReading("", "ai4")).toBe("");
    expect(segmentReading("ài", "")).toBe("ài");
  });

  test("throws rather than guess when the two columns disagree", () => {
    expect(() => segmentReading("nǎa", "na3")).toThrow("unconsumed reading text");
    expect(() => segmentReading("nǐhǎo", "ni3hao3ma5")).toThrow("reading ends before syllable");
    expect(() => segmentReading("nǐhǎo", "ni3ma3")).toThrow("does not match");
  });
});

describe("buildDeck reading field", () => {
  const row = { front: "有时候", reading: "yǒushíhou", back: "sometimes", numeric: "you3shi2hou5" };

  /** The deck's text, read back out of the compiled bytes. */
  function text(bytes: Uint8Array): string {
    return new TextDecoder("utf-8").decode(bytes);
  }

  test("emits separators for a zh deck", async () => {
    const result = await buildDeck([row], { name: "t", slug: "t", lang: "zh" });
    expect(text(result.bytes)).toContain(`yǒu${SEP}shí${SEP}hou`);
  });

  test("leaves the reading alone for a non-zh deck", async () => {
    const result = await buildDeck([row], { name: "t", slug: "t", lang: "fr" });
    expect(text(result.bytes)).toContain("yǒushíhou");
    expect(text(result.bytes)).not.toContain(SEP);
  });

  test("never puts the separator in the glyph set", async () => {
    const result = await buildDeck([row], { name: "t", slug: "t", lang: "zh" });
    expect(result.glyphs).not.toContain(SEP);
  });

  test("fails the whole compile on a row it cannot align", async () => {
    await expect(
      buildDeck([{ ...row, reading: "yǒushíhou", numeric: "you3shi2" }], {
        name: "t",
        slug: "t",
        lang: "zh",
      }),
    ).rejects.toThrow("cannot align pinyin syllables for 有时候");
  });
});
