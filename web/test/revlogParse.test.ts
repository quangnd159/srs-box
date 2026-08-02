import { describe, expect, test } from "bun:test";
import { parseRevlog } from "../lib/revlog/parse";

const MAGIC = "SRSRLOG1";

function makeHeader(version = 1): Uint8Array {
  const buf = new Uint8Array(12);
  buf.set(new TextEncoder().encode(MAGIC), 0);
  new DataView(buf.buffer).setUint32(8, version, true);
  return buf;
}

function makeRecord(opts: {
  cardId: bigint;
  reviewed: bigint;
  rating: number;
  stateBefore: number;
  durationMs: number;
}): Uint8Array {
  const buf = new Uint8Array(24);
  const view = new DataView(buf.buffer);
  view.setBigUint64(0, opts.cardId, true);
  view.setBigInt64(8, opts.reviewed, true);
  buf[16] = opts.rating;
  buf[17] = opts.stateBefore;
  view.setUint16(18, opts.durationMs, true);
  // 20..23 reserved, left zero.
  return buf;
}

function concat(...parts: Uint8Array[]): Uint8Array {
  const total = parts.reduce((n, p) => n + p.length, 0);
  const out = new Uint8Array(total);
  let off = 0;
  for (const p of parts) {
    out.set(p, off);
    off += p.length;
  }
  return out;
}

describe("parseRevlog", () => {
  test("parses a header and complete records", () => {
    const rec1 = makeRecord({ cardId: 1n, reviewed: 1000n, rating: 3, stateBefore: 0, durationMs: 1500 });
    const rec2 = makeRecord({ cardId: 2n, reviewed: 2000n, rating: 1, stateBefore: 2, durationMs: 4000 });
    const bytes = concat(makeHeader(), rec1, rec2);

    const result = parseRevlog(bytes);
    expect(result.version).toBe(1);
    expect(result.truncatedTail).toBe(false);
    expect(result.entries).toEqual([
      { cardId: 1n, reviewed: 1000n, rating: 3, stateBefore: 0, durationMs: 1500 },
      { cardId: 2n, reviewed: 2000n, rating: 1, stateBefore: 2, durationMs: 4000 },
    ]);
  });

  test("tolerates a truncated trailing record", () => {
    const rec1 = makeRecord({ cardId: 1n, reviewed: 1000n, rating: 3, stateBefore: 0, durationMs: 1500 });
    const partial = rec1.subarray(0, 10); // short trailing record
    const bytes = concat(makeHeader(), rec1, partial);

    const result = parseRevlog(bytes);
    expect(result.truncatedTail).toBe(true);
    expect(result.entries).toHaveLength(1);
    expect(result.entries[0].cardId).toBe(1n);
  });

  test("empty file (header only) parses to zero entries", () => {
    const result = parseRevlog(makeHeader());
    expect(result.entries).toHaveLength(0);
    expect(result.truncatedTail).toBe(false);
  });

  test("throws on bad magic", () => {
    const bad = new Uint8Array(12);
    bad.set(new TextEncoder().encode("NOTAMAGIC"), 0);
    expect(() => parseRevlog(bad)).toThrow(/bad magic/);
  });

  test("throws on a buffer shorter than the header", () => {
    expect(() => parseRevlog(new Uint8Array(4))).toThrow(/header/);
  });
});
