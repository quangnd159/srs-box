// Acceptance test: the TS compiler must produce byte-identical .srs output
// to tools/deckc.py for identical input and META keys. Runs the real
// python3 tools/deckc.py as a subprocess and byte-compares.
//
// Skips (rather than fails) if python3 or tools/deckc.py is unavailable, so
// `bun test` still runs in environments without the Python tool.
import { describe, expect, test } from "bun:test";
import { mkdtemp, readFile, rm } from "node:fs/promises";
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
});
