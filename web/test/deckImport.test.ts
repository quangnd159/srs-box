import { describe, expect, test } from "bun:test";
import { importTsv, importWordList } from "../lib/deck/import";

describe("importWordList", () => {
  test("one headword per non-blank line, empty reading/back", () => {
    const cards = importWordList("爱\n八\n\n杯子\n");
    expect(cards.map((c) => c.front)).toEqual(["爱", "八", "杯子"]);
    expect(cards.every((c) => c.reading === "" && c.back === "")).toBe(true);
  });

  test("assigns a unique localId to each card", () => {
    const cards = importWordList("爱\n八\n");
    expect(new Set(cards.map((c) => c.localId)).size).toBe(2);
  });
});

describe("importTsv", () => {
  test("parses the 5-column hskhsk format", () => {
    const { cards, warnings } = importTsv("爱\t愛\tai4\tài\tlove\n");
    expect(cards).toHaveLength(1);
    expect(cards[0]).toMatchObject({ front: "爱", reading: "ài", back: "love" });
    expect(warnings).toEqual([]);
  });

  test("parses the generic 3-column format", () => {
    const { cards } = importTsv("bonjour\tbɔ̃ʒuʁ\thello\n");
    expect(cards[0]).toMatchObject({ front: "bonjour", reading: "bɔ̃ʒuʁ", back: "hello" });
  });

  test("supports an empty reading column", () => {
    const { cards } = importTsv("bonjour\t\thello\n");
    expect(cards[0]).toMatchObject({ front: "bonjour", reading: "", back: "hello" });
  });
});
