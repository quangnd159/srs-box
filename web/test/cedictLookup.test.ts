import { describe, expect, test } from "bun:test";
import { autofill, type CedictTable } from "../lib/cedict/lookup";

describe("autofill", () => {
  const table: CedictTable = {
    你好: [{ pinyin: "ni3 hao3", gloss: "hello; hi" }],
    行: [
      { pinyin: "xing2", gloss: "to walk; ok" },
      { pinyin: "hang2", gloss: "a row; a firm" },
    ],
  };

  test("converts pinyin to diacritics and returns the gloss", () => {
    expect(autofill(table, "你好")).toEqual({ reading: "nǐ hǎo", gloss: "hello; hi" });
  });

  test("picks the first entry for a heteronym", () => {
    expect(autofill(table, "行")).toEqual({ reading: "xíng", gloss: "to walk; ok" });
  });

  test("returns undefined for an unknown headword", () => {
    expect(autofill(table, "不存在")).toBeUndefined();
  });
});
