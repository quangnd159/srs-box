import { describe, expect, test } from "bun:test";
import { stableId } from "../lib/compiler/stableId";

describe("stableId", () => {
  test("is deterministic for the same slug and headword", async () => {
    const a = await stableId("hsk1", "爱");
    const b = await stableId("hsk1", "爱");
    expect(a).toBe(b);
  });

  test("differs when the headword changes", async () => {
    const a = await stableId("hsk1", "爱");
    const b = await stableId("hsk1", "八");
    expect(a).not.toBe(b);
  });

  test("differs when the slug changes", async () => {
    const a = await stableId("hsk1", "爱");
    const b = await stableId("hsk2", "爱");
    expect(a).not.toBe(b);
  });

  test("top bit is always clear", async () => {
    const id = await stableId("hsk1", "爱");
    expect(id <= 0x7fffffffffffffffn).toBe(true);
    expect(id >= 0n).toBe(true);
  });
});
