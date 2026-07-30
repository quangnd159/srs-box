#!/usr/bin/env bun
// Downloads CC-CEDICT and compiles it into a compact JSON lookup table the
// deck editor loads lazily for pinyin/gloss autofill on lang=zh decks.
//
// CC-CEDICT is CC-BY-SA 4.0; see data/CEDICT-LICENSE.txt for the full
// attribution notice, which must ship alongside the compiled JSON.
//
//   bun run scripts/fetch-cedict.ts
//
// Not run automatically by `bun install` (network access + a few seconds of
// parsing); run it once during setup, or whenever CC-CEDICT should refresh.
import { gunzipSync } from "node:zlib";
import { mkdir, writeFile } from "node:fs/promises";
import { join } from "node:path";

const SOURCE_URL = "https://www.mdbg.net/chinese/export/cedict/cedict_1_0_ts_utf-8_mdbg.txt.gz";
const OUT_DIR = join(import.meta.dir, "..", "public", "data");
const OUT_FILE = join(OUT_DIR, "cedict.compact.json");

// traditional simplified [pinyin] /gloss1/gloss2/.../
const LINE_RE = /^(\S+)\s+(\S+)\s+\[([^\]]+)\]\s+\/(.+)\/\s*$/;

export interface CedictEntry {
  /** numeric-tone pinyin, e.g. "ni3 hao3" */
  pinyin: string;
  /** English glosses joined with "; " */
  gloss: string;
}

export function parseCedict(text: string): Record<string, CedictEntry[]> {
  const table: Record<string, CedictEntry[]> = {};
  for (const line of text.split("\n")) {
    if (!line || line.startsWith("#")) continue;
    const match = line.match(LINE_RE);
    if (!match) continue;
    const [, , simplified, pinyin, defs] = match;
    const gloss = defs.split("/").filter(Boolean).join("; ");
    const entry: CedictEntry = { pinyin, gloss };
    (table[simplified] ??= []).push(entry);
  }
  return table;
}

async function main() {
  console.log(`fetching ${SOURCE_URL}`);
  const res = await fetch(SOURCE_URL);
  if (!res.ok) throw new Error(`fetch failed: ${res.status} ${res.statusText}`);
  const gz = new Uint8Array(await res.arrayBuffer());
  const text = gunzipSync(gz).toString("utf-8");

  const table = parseCedict(text);
  const wordCount = Object.keys(table).length;

  await mkdir(OUT_DIR, { recursive: true });
  await writeFile(OUT_FILE, JSON.stringify(table), "utf-8");

  console.log(`${OUT_FILE}`);
  console.log(`  ${wordCount.toLocaleString()} headwords`);
}

if (import.meta.main) {
  main().catch((err) => {
    console.error(err);
    process.exit(1);
  });
}
