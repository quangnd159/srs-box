// Parser for the `.srs` binary deck format: the inverse of build.ts. See
// docs/deck-format.md for the layout this mirrors byte-for-byte.

import { crc32 } from "./crc32";

const MAGIC = "SRSDECK1";
const HEADER_SIZE = 32;
const SECTION_SIZE = 16;
const CARD_SIZE = 32;

export interface DeckHeader {
  totalSize: number;
  sectionCount: number;
  deckId: bigint;
  crc32: number;
}

/** META section as a flat key/value record, e.g. { name, slug, lang, cards }. */
export type DeckMeta = Record<string, string>;

export interface ParsedCard {
  id: bigint;
  front: string;
  back: string;
  /** Syllable-separated with U+001F for lang=zh; verbatim otherwise. See docs/deck-format.md. */
  reading: string;
  tags: number;
}

export interface ParsedDeck {
  header: DeckHeader;
  meta: DeckMeta;
  cards: ParsedCard[];
}

function fail(message: string): never {
  throw new Error(`malformed .srs: ${message}`);
}

function readString(view: DataView, offset: number, length: number): string {
  return new TextDecoder().decode(new Uint8Array(view.buffer, view.byteOffset + offset, length));
}

function parseMeta(text: string): DeckMeta {
  const meta: DeckMeta = {};
  for (const line of text.split("\n")) {
    if (!line) continue;
    const eq = line.indexOf("=");
    if (eq < 0) continue;
    meta[line.slice(0, eq)] = line.slice(eq + 1);
  }
  return meta;
}

/** Parses a `.srs` deck buffer. Throws on bad magic, truncation, or a CRC mismatch. */
export function parseDeck(bytes: Uint8Array): ParsedDeck {
  if (bytes.length < HEADER_SIZE) fail("shorter than the 32-byte header");
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

  const magic = readString(view, 0, 8);
  if (magic !== MAGIC) fail(`bad magic ${JSON.stringify(magic)}, expected ${JSON.stringify(MAGIC)}`);

  const totalSize = view.getUint32(8, true);
  const sectionCount = view.getUint32(12, true);
  const deckId = view.getBigUint64(16, true);
  const crc = view.getUint32(24, true);

  if (totalSize > bytes.length) {
    fail(`header claims ${totalSize} bytes, buffer has ${bytes.length}`);
  }

  const payload = bytes.subarray(HEADER_SIZE, totalSize);
  const actualCrc = crc32(payload);
  if (actualCrc !== crc) {
    fail(`crc32 mismatch: header says ${crc.toString(16)}, computed ${actualCrc.toString(16)}`);
  }

  const tableEnd = HEADER_SIZE + SECTION_SIZE * sectionCount;
  if (tableEnd > totalSize) fail("section table runs past the end of the file");

  let metaBlob: Uint8Array | null = null;
  let cardBlob: Uint8Array | null = null;
  let textBlob: Uint8Array | null = null;

  for (let i = 0; i < sectionCount; i++) {
    const off = HEADER_SIZE + i * SECTION_SIZE;
    const id = readString(view, off, 4);
    const secOffset = view.getUint32(off + 4, true);
    const secLength = view.getUint32(off + 8, true);
    // flags at off + 12, unused today.

    if (secOffset + secLength > totalSize) {
      fail(`section ${JSON.stringify(id)} runs past the end of the file`);
    }
    const blob = bytes.subarray(secOffset, secOffset + secLength);

    // Unknown section ids are skipped, per docs/deck-format.md, so a newer
    // compiler can push to older firmware (and vice versa here) without
    // this parser breaking.
    if (id === "META") metaBlob = blob;
    else if (id === "CARD") cardBlob = blob;
    else if (id === "TEXT") textBlob = blob;
  }

  if (!metaBlob) fail("missing META section");
  if (!cardBlob) fail("missing CARD section");
  if (!textBlob) fail("missing TEXT section");
  if (cardBlob.length % CARD_SIZE !== 0) {
    fail(`CARD section length ${cardBlob.length} is not a multiple of ${CARD_SIZE}`);
  }

  const meta = parseMeta(new TextDecoder().decode(metaBlob));
  const textDecoder = new TextDecoder();

  const cardCount = cardBlob.length / CARD_SIZE;
  const cardView = new DataView(cardBlob.buffer, cardBlob.byteOffset, cardBlob.byteLength);
  const cards: ParsedCard[] = [];
  for (let i = 0; i < cardCount; i++) {
    const off = i * CARD_SIZE;
    const id = cardView.getBigUint64(off + 0, true);
    const frontOff = cardView.getUint32(off + 8, true);
    const frontLen = cardView.getUint16(off + 12, true);
    const backOff = cardView.getUint32(off + 14, true);
    const backLen = cardView.getUint16(off + 18, true);
    const readingOff = cardView.getUint32(off + 20, true);
    const readingLen = cardView.getUint16(off + 24, true);
    const tags = cardView.getUint16(off + 26, true);

    const sliceText = (o: number, len: number): string => {
      if (len === 0) return "";
      if (o + len > textBlob!.length) fail(`card ${id} text range runs past TEXT section`);
      return textDecoder.decode(textBlob!.subarray(o, o + len));
    };

    cards.push({
      id,
      front: sliceText(frontOff, frontLen),
      back: sliceText(backOff, backLen),
      reading: sliceText(readingOff, readingLen),
      tags,
    });
  }

  return {
    header: { totalSize, sectionCount, deckId, crc32: crc },
    meta,
    cards,
  };
}
