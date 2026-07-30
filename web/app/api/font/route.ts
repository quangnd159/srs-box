// Subsets Noto Sans SC (CJK) and Noto Sans (Latin/IPA/Vietnamese) down to
// the glyphs actually used across all decks (plus the fixed UI set) and
// emits LVGL binary fonts at every size the firmware ships. See
// lib/font/subset.ts for the conversion logic and why two font sources are
// needed; this route only handles the HTTP request/response shape.
//
// Node runtime (not Edge): lv_font_conv loads a WASM FreeType build from
// disk and reads the source fonts from the filesystem.
import { join } from "node:path";
import { NextRequest, NextResponse } from "next/server";
import { subsetFontAllSizes, unionGlyphs, type FontPaths } from "@/lib/font/subset";

export const runtime = "nodejs";

const FONTS: FontPaths = {
  cjkFontPath: join(process.cwd(), "assets", "fonts", "NotoSansSC-Regular.ttf"),
  latinFontPath: join(process.cwd(), "assets", "fonts", "NotoSans-Regular.ttf"),
};

interface FontRequestBody {
  /** Each deck's glyph set (its TEXT content, deduped); union is taken here. */
  deckGlyphSets: string[];
}

export async function POST(req: NextRequest): Promise<NextResponse> {
  let body: FontRequestBody;
  try {
    body = await req.json();
  } catch {
    return NextResponse.json({ error: "invalid JSON body" }, { status: 400 });
  }

  if (!Array.isArray(body.deckGlyphSets)) {
    return NextResponse.json({ error: "expected { deckGlyphSets: string[] }" }, { status: 400 });
  }

  const symbols = unionGlyphs(body.deckGlyphSets);

  let sizes: Map<number, Uint8Array>;
  try {
    sizes = await subsetFontAllSizes(FONTS, symbols);
  } catch (err) {
    return NextResponse.json(
      { error: `font subsetting failed: ${(err as Error).message}` },
      { status: 500 },
    );
  }

  const fonts: Record<string, string> = {};
  for (const [size, bin] of sizes) {
    fonts[String(size)] = Buffer.from(bin).toString("base64");
  }

  return NextResponse.json({ fonts });
}
