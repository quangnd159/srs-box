// Parser for revlog.bin, matching firmware/components/persist/include/persist.h
// byte-for-byte: a 12-byte header (magic + version) followed by 24-byte
// ReviewEntry records. See docs/deck-format.md.

const MAGIC = "SRSRLOG1";
const HEADER_SIZE = 12; // magic[8] + version u32
const RECORD_SIZE = 24; // card_id(8) reviewed(8) rating(1) state(1) duration(2) reserved(4)

export interface ReviewEntry {
  cardId: bigint;
  /** Unix seconds, UTC. */
  reviewed: bigint;
  /** 1 Again, 2 Hard, 3 Good, 4 Easy. */
  rating: number;
  /** Card state before the review (0 new, 1 learning, 2 review, 3 relearning). */
  stateBefore: number;
  durationMs: number;
}

export interface ParsedRevlog {
  version: number;
  entries: ReviewEntry[];
  /** True when the file ended partway through a record (e.g. a power cut mid-append). */
  truncatedTail: boolean;
}

function fail(message: string): never {
  throw new Error(`malformed revlog.bin: ${message}`);
}

/** Parses a revlog.bin buffer. Throws on a missing/bad magic; tolerates a truncated trailing record. */
export function parseRevlog(bytes: Uint8Array): ParsedRevlog {
  if (bytes.length < HEADER_SIZE) fail("shorter than the 12-byte header");

  const magic = new TextDecoder().decode(bytes.subarray(0, 8));
  if (magic !== MAGIC) fail(`bad magic ${JSON.stringify(magic)}, expected ${JSON.stringify(MAGIC)}`);

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const version = view.getUint32(8, true);

  const body = bytes.length - HEADER_SIZE;
  const completeCount = Math.floor(body / RECORD_SIZE);
  const truncatedTail = body % RECORD_SIZE !== 0;

  const entries: ReviewEntry[] = [];
  for (let i = 0; i < completeCount; i++) {
    const off = HEADER_SIZE + i * RECORD_SIZE;
    entries.push({
      cardId: view.getBigUint64(off + 0, true),
      reviewed: view.getBigInt64(off + 8, true),
      rating: view.getUint8(off + 16),
      stateBefore: view.getUint8(off + 17),
      durationMs: view.getUint16(off + 18, true),
      // off + 20..23: reserved, ignored, matching persist.h's decode().
    });
  }

  return { version, entries, truncatedTail };
}
