import { describe, expect, test } from "bun:test";
import { TextPool } from "../lib/compiler/textPool";

describe("TextPool", () => {
  test("empty string maps to offset 0, length 0 without consuming space", () => {
    const pool = new TextPool();
    expect(pool.add("")).toEqual([0, 0]);
    expect(pool.byteLength).toBe(0);
  });

  test("dedups repeated strings to the same offset", () => {
    const pool = new TextPool();
    const first = pool.add("你好");
    const second = pool.add("你好");
    expect(first).toEqual(second);
    expect(pool.uniqueCount).toBe(1);
  });

  test("distinct strings get distinct, sequential offsets", () => {
    const pool = new TextPool();
    const [off1, len1] = pool.add("abc");
    const [off2] = pool.add("defg");
    expect(off1).toBe(0);
    expect(len1).toBe(3);
    expect(off2).toBe(3);
    expect(pool.byteLength).toBe(7);
  });

  test("toBytes concatenates in insertion order", () => {
    const pool = new TextPool();
    pool.add("ab");
    pool.add("cd");
    expect(new TextDecoder().decode(pool.toBytes())).toBe("abcd");
  });
});
