// Font subsetting core, kept free of Next.js request/response types so it
// can be unit tested directly with `bun test` (no dev server needed) and
// reused by the /api/font route handler.
//
// Wraps lv_font_conv's JS conversion pipeline (lv_font_conv/lib/convert),
// mirroring tools/buildfont.sh's invocation: --format bin --bpp 4
// --no-compress, at sizes 16/20/28/48.
//
// Two font sources, because no single Noto face covers everything a deck
// needs: Noto Sans SC has the hanzi but NO IPA (confirmed empirically —
// U+0254/U+0292/U+0281 all map to glyph 0, i.e. .notdef — so a French
// deck's IPA readings would silently render as blank tofu boxes), while
// plain Noto Sans has IPA, Vietnamese, and the middle dot but no CJK.
// lv_font_conv merges ranges from multiple --font args, same as the CLI
// script does; this must stay in sync with tools/buildfont.sh so a deck
// looks the same whether its fonts were built by the web app or the CLI.
import { readFile } from "node:fs/promises";

// eslint-disable-next-line @typescript-eslint/no-require-imports
const convert = require("lv_font_conv/lib/convert");

export const FONT_SIZES = [16, 20, 28, 48] as const;

// The CJK cutoff (U+2E80, start of the CJK-adjacent blocks): nothing a deck
// uses between IPA/Vietnamese Latin and there is in Noto Sans SC. Matches
// tools/buildfont.sh.
const CJK_CUTOFF = 0x2e80;

// UI glyphs the decks themselves may not contain. Without these, anything
// the firmware draws that is not deck text renders as a tofu box: U+2713
// check mark (from the SC face, which has it; plain Noto Sans does not),
// U+00B7 middle dot (from the Latin face). Any future UI character must be
// added here — a missing glyph fails silently as an empty rectangle rather
// than as an error.
export const UI_SYMBOLS = "✓·";

type FontRange = { symbols: string } | { range: [number, number, number] };

interface LvFontConvArgs {
  size: number;
  bpp: 4;
  format: "bin";
  no_compress: boolean;
  no_prefilter: boolean;
  no_kerning: boolean;
  fast_kerning: boolean;
  use_color_info: boolean;
  lcd: boolean;
  lcd_v: boolean;
  full_info: boolean;
  output: string;
  font: Array<{
    source_path: string;
    source_bin: Buffer;
    ranges: FontRange[];
  }>;
}

export interface FontPaths {
  /** Noto Sans SC: CJK glyphs plus the check mark. */
  cjkFontPath: string;
  /** Plain Noto Sans: Latin/IPA/Vietnamese glyphs plus the middle dot. */
  latinFontPath: string;
}

/** Splits a glyph set into the Latin/IPA run and the CJK run, per CJK_CUTOFF. */
export function splitByScript(glyphs: string): { latin: string; cjk: string } {
  const latin: string[] = [];
  const cjk: string[] = [];
  for (const ch of new Set(Array.from(glyphs))) {
    const cp = ch.codePointAt(0) ?? 0;
    // Control characters have no glyph: the compiler's syllable separator
    // (U+001F, see lib/compiler/pinyin.ts) must never be requested here.
    if (cp < 0x20) continue;
    if (cp >= CJK_CUTOFF) cjk.push(ch);
    else latin.push(ch);
  }
  latin.sort();
  cjk.sort();
  return { latin: latin.join(""), cjk: cjk.join("") };
}

/** Subsets both font sources to `symbols` at one pixel size, LVGL --format bin. */
export async function subsetFont(
  fonts: FontPaths,
  size: number,
  symbols: string,
): Promise<Uint8Array> {
  const { latin, cjk } = splitByScript(symbols);
  const [cjkBin, latinBin] = await Promise.all([
    readFile(fonts.cjkFontPath),
    readFile(fonts.latinFontPath),
  ]);

  const outputKey = `font_cjk_${size}.bin`;

  const latinRanges: FontRange[] = [{ range: [0x20, 0x7f, 0x20] }, { range: [0x00b7, 0x00b7, 0x00b7] }];
  if (latin) latinRanges.push({ symbols: latin });

  const cjkRanges: FontRange[] = [{ range: [0x2713, 0x2713, 0x2713] }];
  if (cjk) cjkRanges.push({ symbols: cjk });

  const args: LvFontConvArgs = {
    size,
    bpp: 4,
    format: "bin",
    no_compress: true,
    no_prefilter: false,
    no_kerning: false,
    fast_kerning: false,
    use_color_info: false,
    lcd: false,
    lcd_v: false,
    full_info: false,
    output: outputKey,
    font: [
      { source_path: fonts.latinFontPath, source_bin: latinBin, ranges: latinRanges },
      { source_path: fonts.cjkFontPath, source_bin: cjkBin, ranges: cjkRanges },
    ],
  };

  const files = (await convert(args)) as Record<string, Uint8Array | Buffer>;
  const data = files[outputKey];
  if (!data) throw new Error(`lv_font_conv produced no output for size ${size}`);
  return data instanceof Uint8Array ? data : new Uint8Array(data);
}

/** Union of glyphs from every deck's text plus the fixed UI symbols, deduped. */
export function unionGlyphs(deckGlyphSets: string[]): string {
  const set = new Set<string>();
  for (const glyphs of deckGlyphSets) {
    for (const ch of glyphs) set.add(ch);
  }
  for (const ch of UI_SYMBOLS) set.add(ch);
  return Array.from(set).join("");
}

/** Subsets both font sources at every firmware font size. Returns size -> binary. */
export async function subsetFontAllSizes(
  fonts: FontPaths,
  symbols: string,
): Promise<Map<number, Uint8Array>> {
  const out = new Map<number, Uint8Array>();
  for (const size of FONT_SIZES) {
    out.set(size, await subsetFont(fonts, size, symbols));
  }
  return out;
}
