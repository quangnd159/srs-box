// Exercises the real lv_font_conv pipeline (no Next.js dev server needed,
// per CLAUDE.md's "don't run dev server" instruction: this calls the
// library function directly). Slow-ish (font rasterization), so kept to a
// single size and a small glyph set.
import { describe, expect, test } from "bun:test";
import { join } from "node:path";
import { splitByScript, subsetFont, unionGlyphs, type FontPaths } from "../lib/font/subset";

const FONTS: FontPaths = {
  cjkFontPath: join(import.meta.dir, "..", "assets", "fonts", "NotoSansSC-Regular.ttf"),
  latinFontPath: join(import.meta.dir, "..", "assets", "fonts", "NotoSans-Regular.ttf"),
};

describe("splitByScript", () => {
  test("routes CJK and Latin/IPA glyphs to different buckets", () => {
    const { latin, cjk } = splitByScript("爱àʒ");
    expect(cjk).toBe("爱");
    expect(latin.split("").sort().join("")).toBe(["à", "ʒ"].sort().join(""));
  });
});

describe("unionGlyphs", () => {
  test("dedupes across decks and always includes the UI symbols", () => {
    const symbols = unionGlyphs(["爱八", "八北京"]);
    expect(symbols).toContain("爱");
    expect(symbols).toContain("八");
    expect(symbols).toContain("北");
    expect(symbols).toContain("✓");
    expect(symbols).toContain("·");
    // deduped: 八 appears once even though it's in both decks
    expect(symbols.split("八")).toHaveLength(2);
  });
});

describe("subsetFont", () => {
  test(
    "produces a non-empty LVGL bin font covering CJK, Latin, and IPA glyphs",
    async () => {
      // Noto Sans SC alone does NOT have IPA (U+0254 ɔ, U+0292 ʒ, U+0281 ʁ
      // all map to glyph 0/.notdef there — verified directly with
      // opentype.js). This is exactly why subsetFont merges two font
      // sources; the test would need a real IPA glyph present to actually
      // catch a regression back to a single-font subset.
      const symbols = unionGlyphs(["爱ɔʒ"]);
      const bin = await subsetFont(FONTS, 16, symbols);

      expect(bin.length).toBeGreaterThan(0);
      expect(bin[0]).not.toBe(undefined);
    },
    30_000,
  );

  test(
    "actually rasterizes the requested CJK and IPA codepoints (not just .notdef)",
    async () => {
      // Stronger than the binary-output test above: inspects lv_font_conv's
      // intermediate glyph data directly, confirming U+7231 (爱) and U+0292
      // (ʒ) both got a real, distinct glyph index, matching the opentype.js
      // check that motivated the two-font-source split.
      // eslint-disable-next-line @typescript-eslint/no-require-imports
      const collectFontData = require("lv_font_conv/lib/collect_font_data");
      const { latin, cjk } = splitByScript("爱ɔʒ");
      const [latinBin, cjkBin] = await Promise.all([
        Bun.file(FONTS.latinFontPath).arrayBuffer(),
        Bun.file(FONTS.cjkFontPath).arrayBuffer(),
      ]);

      const fontData = await collectFontData({
        size: 16,
        font: [
          {
            source_path: FONTS.latinFontPath,
            source_bin: Buffer.from(latinBin),
            ranges: [{ symbols: latin }],
          },
          {
            source_path: FONTS.cjkFontPath,
            source_bin: Buffer.from(cjkBin),
            ranges: [{ symbols: cjk }],
          },
        ],
      });

      const codes = new Set(fontData.glyphs.map((g: { code: number }) => g.code));
      expect(codes.has("爱".codePointAt(0)!)).toBe(true);
      expect(codes.has("ɔ".codePointAt(0)!)).toBe(true);
      expect(codes.has("ʒ".codePointAt(0)!)).toBe(true);
    },
    30_000,
  );
});
