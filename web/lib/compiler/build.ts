// TypeScript port of tools/deckc.py's `build` command. Must produce
// byte-identical .srs output to the Python compiler for identical input and
// META keys; see docs/deck-format.md for the binary layout and
// web/test/deckc.test.ts for the acceptance test that checks this.

import { crc32 } from "./crc32";
import { stableId } from "./stableId";
import { TextPool } from "./textPool";
import type { BuildOptions, BuildResult, DeckRow } from "./types";

const MAGIC = "SRSDECK1"; // 8 bytes ASCII
const HEADER_SIZE = 32;
const SECTION_SIZE = 16;
const CARD_SIZE = 32;

// CJK Unified Ideographs block, matching deckc.py's "一" <= g <= "鿿" check.
const CJK_LOW = "一".codePointAt(0)!; // U+4E00
const CJK_HIGH = "鿿".codePointAt(0)!; // U+9FFF

interface PooledCard {
  id: bigint;
  front: [number, number];
  back: [number, number];
  reading: [number, number];
}

async function assignIds(rows: DeckRow[], slug: string): Promise<bigint[]> {
  return Promise.all(rows.map((r) => stableId(slug, r.front)));
}

/** Compiles rows into a `.srs` bundle. See docs/deck-format.md. */
export async function buildDeck(rows: DeckRow[], opts: BuildOptions): Promise<BuildResult> {
  if (rows.length === 0) {
    throw new Error("no usable rows");
  }

  const ids = await assignIds(rows, opts.slug);
  const text = new TextPool();
  const pooled: PooledCard[] = rows.map((row, i) => ({
    id: ids[i],
    front: text.add(row.front),
    back: text.add(row.back),
    reading: text.add(row.reading),
  }));

  // The device binary-searches this section, so it must be sorted by id.
  pooled.sort((a, b) => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0));
  const idSet = new Set(pooled.map((c) => c.id.toString()));
  if (idSet.size !== pooled.length) {
    throw new Error("card id collision; this should be impossible with sha256");
  }

  const cardBlob = new Uint8Array(CARD_SIZE * pooled.length);
  const cardView = new DataView(cardBlob.buffer);
  pooled.forEach((c, i) => {
    const off = i * CARD_SIZE;
    cardView.setBigUint64(off + 0, c.id, true);
    cardView.setUint32(off + 8, c.front[0], true);
    cardView.setUint16(off + 12, c.front[1], true);
    cardView.setUint32(off + 14, c.back[0], true);
    cardView.setUint16(off + 18, c.back[1], true);
    cardView.setUint32(off + 20, c.reading[0], true);
    cardView.setUint16(off + 24, c.reading[1], true);
    cardView.setUint16(off + 26, 0, true); // tags
    cardView.setUint32(off + 28, 0, true); // reserved
  });

  const lang = opts.lang ?? "zh"; // matches deckc.py's --lang default
  const metaText = `name=${opts.name}\nslug=${opts.slug}\ncards=${pooled.length}\nlang=${lang}\n`;
  const metaBlob = new TextEncoder().encode(metaText);
  const textBlob = text.toBytes();

  const sections: Array<{ id: string; blob: Uint8Array }> = [
    { id: "META", blob: metaBlob },
    { id: "CARD", blob: cardBlob },
    { id: "TEXT", blob: textBlob },
  ];

  const bodyStart = HEADER_SIZE + SECTION_SIZE * sections.length;
  const payloadLength =
    sections.reduce((sum, s) => sum + s.blob.length, 0) + SECTION_SIZE * sections.length;
  const payload = new Uint8Array(payloadLength);
  const payloadView = new DataView(payload.buffer);

  let tableOffset = 0;
  let bodyOffset = SECTION_SIZE * sections.length; // relative to payload start
  let fileOffset = bodyStart;
  for (const s of sections) {
    const idBytes = new TextEncoder().encode(s.id);
    payload.set(idBytes, tableOffset);
    payloadView.setUint32(tableOffset + 4, fileOffset, true);
    payloadView.setUint32(tableOffset + 8, s.blob.length, true);
    payloadView.setUint32(tableOffset + 12, 0, true); // flags
    tableOffset += SECTION_SIZE;

    payload.set(s.blob, bodyOffset);
    bodyOffset += s.blob.length;
    fileOffset += s.blob.length;
  }

  const totalSize = HEADER_SIZE + payload.length;
  const crc = crc32(payload);
  const deckId = await stableId(opts.slug, "\x00deck");

  const header = new Uint8Array(HEADER_SIZE);
  const headerView = new DataView(header.buffer);
  header.set(new TextEncoder().encode(MAGIC), 0);
  headerView.setUint32(8, totalSize, true);
  headerView.setUint32(12, sections.length, true);
  headerView.setBigUint64(16, deckId, true);
  headerView.setUint32(24, crc, true);
  // bytes 28-31: 4x pad, left zero.

  const out = new Uint8Array(totalSize);
  out.set(header, 0);
  out.set(payload, HEADER_SIZE);

  const glyphSet = new Set<string>();
  for (const r of rows) {
    for (const ch of r.front + r.reading + r.back) glyphSet.add(ch);
  }
  const glyphs = Array.from(glyphSet).sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
  const cjkGlyphs = glyphs.filter((g) => {
    const cp = g.codePointAt(0)!;
    return cp >= CJK_LOW && cp <= CJK_HIGH;
  });

  return {
    bytes: out,
    cardCount: pooled.length,
    textPoolBytes: text.byteLength,
    uniqueStrings: text.uniqueCount,
    glyphs,
    cjkGlyphs,
  };
}
