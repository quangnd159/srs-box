// Acceptance test: the TS compiler must produce byte-identical .srs output
// to tools/deckc.py for identical input and META keys. Runs the real
// python3 tools/deckc.py as a subprocess and byte-compares.
//
// Skips (rather than fails) if python3 or tools/deckc.py is unavailable, so
// `bun test` still runs in environments without the Python tool.
import { describe, expect, test } from "bun:test";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { spawnSync } from "node:child_process";

import { parseTsv } from "../lib/compiler/parse";
import { buildDeck } from "../lib/compiler/build";

const REPO_ROOT = resolve(import.meta.dir, "../..");
const DECKC = join(REPO_ROOT, "tools", "deckc.py");
const FIXTURE = join(import.meta.dir, "fixtures", "hsk1-2-vi.raw.tsv");

function pythonAvailable(): boolean {
  const probe = spawnSync("python3", ["--version"]);
  return probe.status === 0;
}

describe("byte-identity with tools/deckc.py", () => {
  test("compiles hsk1-2-vi.raw.tsv to the exact same bytes as the python compiler", async () => {
    if (!pythonAvailable()) {
      console.warn("python3 not available; skipping acceptance test");
      return;
    }

    const name = "HSK 1-2";
    const id = "hsk1-2-vi";

    const dir = await mkdtemp(join(tmpdir(), "srsbox-acceptance-"));
    try {
      const pyOut = join(dir, "python.srs");
      const result = spawnSync(
        "python3",
        [DECKC, "build", FIXTURE, "-o", pyOut, "--name", name, "--id", id, "--lang", "zh"],
        { encoding: "utf-8" },
      );
      expect(result.status).toBe(0);

      const pyBytes = new Uint8Array(await readFile(pyOut));

      const source = await readFile(FIXTURE, "utf-8");
      const { rows } = parseTsv(source);
      const tsResult = await buildDeck(rows, { name, slug: id, lang: "zh" });

      expect(tsResult.bytes.length).toBe(pyBytes.length);
      expect(Buffer.from(tsResult.bytes).equals(Buffer.from(pyBytes))).toBe(true);
    } finally {
      await rm(dir, { recursive: true, force: true });
    }
  });

  // The syllable separators (docs/deck-format.md) live in the text pool, so
  // any disagreement about where a syllable ends is a byte difference here.
  // The rows are the awkward ones: a coda after the tone mark, a neutral
  // final, ü in all three spellings, erhua, and two alternate readings.
  test("agrees on syllable segmentation, including codas, ü and erhua", async () => {
    if (!pythonAvailable()) {
      console.warn("python3 not available; skipping acceptance test");
      return;
    }

    const tsv =
      [
        ["有时候", "有時候", "you3shi2hou5", "yǒushíhou", "sometimes"],
        ["爸爸", "爸爸", "ba4ba5", "bàba", "dad"],
        ["旅游", "旅遊", "lü3you2", "lǚyóu", "to travel"],
        ["忽略", "忽略", "hu1lu:e4", "hūlüè", "to overlook"],
        ["女孩儿", "女孩兒", "nü3hai2r5", "nǚháir", "girl"],
        ["长", "長", "chang2, zhang3", "cháng, zhǎng", "long; to grow"],
      ]
        .map((cols) => cols.join("\t"))
        .join("\n") + "\n";

    const name = "Segmentation";
    const id = "seg";
    const dir = await mkdtemp(join(tmpdir(), "srsbox-acceptance-"));
    try {
      const src = join(dir, "seg.tsv");
      await writeFile(src, tsv, "utf-8");
      const pyOut = join(dir, "python.srs");
      const result = spawnSync(
        "python3",
        [DECKC, "build", src, "-o", pyOut, "--name", name, "--id", id, "--lang", "zh"],
        { encoding: "utf-8" },
      );
      expect(result.status).toBe(0);

      const pyBytes = new Uint8Array(await readFile(pyOut));
      const { rows } = parseTsv(tsv);
      const tsResult = await buildDeck(rows, { name, slug: id, lang: "zh" });

      expect(Buffer.from(tsResult.bytes).equals(Buffer.from(pyBytes))).toBe(true);
      // And the separators really are in there, not absent from both.
      expect(new TextDecoder().decode(pyBytes)).toContain("yǒushíhou");
    } finally {
      await rm(dir, { recursive: true, force: true });
    }
  });

  test("both compilers refuse a row whose columns disagree", async () => {
    if (!pythonAvailable()) {
      console.warn("python3 not available; skipping acceptance test");
      return;
    }

    const tsv = "哪\t哪\tna3\tnǎa\twhich\n";
    const dir = await mkdtemp(join(tmpdir(), "srsbox-acceptance-"));
    try {
      const src = join(dir, "bad.tsv");
      await writeFile(src, tsv, "utf-8");
      const result = spawnSync(
        "python3",
        [DECKC, "build", src, "-o", join(dir, "bad.srs"), "--name", "x", "--id", "x", "--lang", "zh"],
        { encoding: "utf-8" },
      );
      expect(result.status).not.toBe(0);
      expect(result.stderr).toContain("cannot align pinyin syllables");

      const { rows } = parseTsv(tsv);
      await expect(buildDeck(rows, { name: "x", slug: "x", lang: "zh" })).rejects.toThrow(
        "cannot align pinyin syllables",
      );
    } finally {
      await rm(dir, { recursive: true, force: true });
    }
  });
});
