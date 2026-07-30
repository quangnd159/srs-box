import { describe, expect, test } from "bun:test";
import { crc32 } from "../lib/compiler/crc32";

describe("crc32", () => {
  test("matches known zlib.crc32 vectors", () => {
    expect(crc32(new TextEncoder().encode(""))).toBe(0);
    expect(crc32(new TextEncoder().encode("The quick brown fox jumps over the lazy dog"))).toBe(
      0x414fa339,
    );
    expect(crc32(new TextEncoder().encode("123456789"))).toBe(0xcbf43926);
  });
});
