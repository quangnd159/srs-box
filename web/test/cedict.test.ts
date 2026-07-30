import { describe, expect, test } from "bun:test";
import { parseCedict } from "../scripts/fetch-cedict";

const SAMPLE = `# CC-CEDICT
# comment line
爸爸 爸爸 [ba4 ba5] /dad/father/daddy (informal)/
你好 你好 [ni3 hao3] /hello/hi/
銀行 银行 [yin2 hang2] /bank/
`;

describe("parseCedict", () => {
  test("parses simplified headword, pinyin, and joined gloss", () => {
    const table = parseCedict(SAMPLE);
    expect(table["爸爸"]).toEqual([{ pinyin: "ba4 ba5", gloss: "dad; father; daddy (informal)" }]);
  });

  test("ignores comment lines", () => {
    const table = parseCedict(SAMPLE);
    expect(Object.keys(table)).not.toContain("#");
  });

  test("keys by the simplified form, not traditional", () => {
    const table = parseCedict(SAMPLE);
    expect(table["银行"]).toEqual([{ pinyin: "yin2 hang2", gloss: "bank" }]);
    expect(table["銀行"]).toBeUndefined();
  });

  test("accumulates multiple entries for the same headword", () => {
    const table = parseCedict(
      "行 行 [xing2] /to walk/\n行 行 [hang2] /a row/\n",
    );
    expect(table["行"]).toHaveLength(2);
  });
});
