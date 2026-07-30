import { describe, expect, test } from "bun:test";
import { parseTsv } from "../lib/compiler/parse";

describe("parseTsv: 5-column hskhsk format", () => {
  test("parses simplified/traditional/numeric/pinyin/gloss", () => {
    const { rows } = parseTsv("爱\t愛\tai4\tài\tyêu; yêu thích\n");
    expect(rows).toEqual([{ front: "爱", reading: "ài", back: "yêu; yêu thích" }]);
  });

  test("detects the shape from the first row and skips short later rows", () => {
    const { rows, warnings } = parseTsv("爱\t愛\tai4\tài\tgloss\na\tb\tc\n");
    expect(rows).toHaveLength(1);
    expect(warnings).toHaveLength(1);
    expect(warnings[0].message).toBe("expected 5 fields, got 3");
  });

  test("skips duplicate headwords, keeping the first", () => {
    const { rows, warnings } = parseTsv("爱\t愛\tai4\tài\tfirst\n爱\t愛\tai4\tài\tsecond\n");
    expect(rows).toEqual([{ front: "爱", reading: "ài", back: "first" }]);
    expect(warnings).toHaveLength(1);
  });

  test("ignores blank lines", () => {
    const { rows } = parseTsv("爱\t愛\tai4\tài\tgloss\n\n\n");
    expect(rows).toHaveLength(1);
  });

  test("strips a leading BOM", () => {
    const { rows } = parseTsv("﻿爱\t愛\tai4\tài\tgloss\n");
    expect(rows[0].front).toBe("爱");
  });
});

describe("parseTsv: 3-column generic format", () => {
  test("parses front/reading/back", () => {
    const { rows } = parseTsv("bonjour\tbɔ̃ʒuʁ\thello\n");
    expect(rows).toEqual([{ front: "bonjour", reading: "bɔ̃ʒuʁ", back: "hello" }]);
  });

  test("allows an empty reading column", () => {
    const { rows } = parseTsv("bonjour\t\thello\n");
    expect(rows).toEqual([{ front: "bonjour", reading: "", back: "hello" }]);
  });

  test("a 2-field first row locks in the 3-column shape and is itself skipped", () => {
    // Matches tools/deckc.py's read_rows(): the first row decides the
    // shape (< 5 fields => 3-column), then every row including the first
    // is checked against that shape, so an under-length first row is
    // dropped rather than special-cased.
    const { rows, warnings } = parseTsv("bonjour\thello\nchat\tʃa\tcat\n");
    expect(rows).toEqual([{ front: "chat", reading: "ʃa", back: "cat" }]);
    expect(warnings).toHaveLength(1);
  });
});
