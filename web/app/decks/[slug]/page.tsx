"use client";

import { use, useState } from "react";
import { buildDeck } from "@/lib/compiler/build";
import { autofill, loadCedict, type CedictTable } from "@/lib/cedict/lookup";
import { importTsv, importWordList } from "@/lib/deck/import";
import type { Card, Deck } from "@/lib/deck/types";
import { isValidSlug } from "@/lib/deck/types";
import { useDeckStore } from "@/lib/deck/useDeckStore";

function newLocalId(): string {
  return crypto.randomUUID();
}

export default function DeckEditorPage({ params }: { params: Promise<{ slug: string }> }) {
  const { slug } = use(params);
  const store = useDeckStore();
  // Computed fresh each render (a cheap synchronous localStorage read); the
  // only way this page changes decks is a full navigation (see
  // handleChangeSlug below), so it doesn't need to be React state.
  const deck: Deck | undefined = store.getDeck(slug);
  const [cards, setCards] = useState<Card[]>(() => store.getCards(slug));
  const [pasteText, setPasteText] = useState("");
  const [importWarnings, setImportWarnings] = useState<string[]>([]);
  const [cedict, setCedict] = useState<CedictTable | null>(null);
  const [newSlug, setNewSlug] = useState("");
  const [status, setStatus] = useState<string | null>(null);

  function persist(next: Card[]) {
    setCards(next);
    store.setCards(slug, next);
  }

  function updateCard(localId: string, patch: Partial<Card>) {
    persist(cards.map((c) => (c.localId === localId ? { ...c, ...patch } : c)));
  }

  function removeCard(localId: string) {
    persist(cards.filter((c) => c.localId !== localId));
  }

  function addBlankRow() {
    persist([...cards, { localId: newLocalId(), front: "", reading: "", back: "" }]);
  }

  function handleImport(mode: "wordlist" | "tsv") {
    if (!pasteText.trim()) return;
    if (mode === "wordlist") {
      persist([...cards, ...importWordList(pasteText)]);
      setImportWarnings([]);
    } else {
      const { cards: imported, warnings } = importTsv(pasteText);
      persist([...cards, ...imported]);
      setImportWarnings(warnings);
    }
    setPasteText("");
  }

  async function handleAutofillAll() {
    const table = cedict ?? (await loadCedict());
    if (!cedict) setCedict(table);
    persist(
      cards.map((c) => {
        if (c.reading && c.back) return c;
        const result = autofill(table, c.front);
        if (!result) return c;
        return {
          ...c,
          reading: c.reading || result.reading,
          back: c.back || result.gloss,
        };
      }),
    );
  }

  async function handleCompileDownload() {
    if (!deck) return;
    setStatus("compiling...");
    try {
      const result = await buildDeck(
        cards.filter((c) => c.front.trim()),
        { name: deck.name, slug: deck.slug, lang: deck.lang },
      );
      const blob = new Blob([result.bytes.buffer as ArrayBuffer], {
        type: "application/octet-stream",
      });
      const url = URL.createObjectURL(blob);
      const a = document.createElement("a");
      a.href = url;
      a.download = `${deck.slug}.srs`;
      a.click();
      URL.revokeObjectURL(url);
      setStatus(`compiled ${result.cardCount} cards, ${result.bytes.length.toLocaleString()} bytes`);
    } catch (err) {
      setStatus(`compile failed: ${(err as Error).message}`);
    }
  }

  function handleChangeSlug() {
    if (!newSlug || !isValidSlug(newSlug)) {
      setStatus("enter a valid slug (lowercase letters, digits, hyphens)");
      return;
    }
    if (
      !confirm(
        "Changing the slug orphans this deck's review history on the device: every card gets a new id. Continue?",
      )
    ) {
      return;
    }
    const updated = store.changeSlug(slug, newSlug);
    window.location.href = `/decks/${updated.slug}`;
  }

  if (!deck) {
    return (
      <div className="mx-auto max-w-3xl px-6 py-10">
        <p className="text-sm text-zinc-500">No such deck: {slug}</p>
      </div>
    );
  }

  return (
    <div className="mx-auto max-w-4xl px-6 py-10">
      <h1 className="text-2xl font-semibold">{deck.name}</h1>
      <p className="mt-1 text-sm text-zinc-500">
        slug: {deck.slug} · lang: {deck.lang} · {cards.length} cards
      </p>

      <section className="mt-6 rounded-lg border border-zinc-200 p-4 dark:border-zinc-800">
        <h2 className="text-sm font-semibold">Import</h2>
        <textarea
          className="mt-2 h-28 w-full rounded border border-zinc-300 bg-white p-2 font-mono text-sm dark:border-zinc-700 dark:bg-zinc-900"
          placeholder={
            "Paste a word list (one per line), or a TSV: front\\treading\\tback"
          }
          value={pasteText}
          onChange={(e) => setPasteText(e.target.value)}
        />
        <div className="mt-2 flex gap-2">
          <button
            onClick={() => handleImport("wordlist")}
            className="rounded border border-zinc-300 px-3 py-1 text-xs dark:border-zinc-700"
          >
            Add as word list
          </button>
          <button
            onClick={() => handleImport("tsv")}
            className="rounded border border-zinc-300 px-3 py-1 text-xs dark:border-zinc-700"
          >
            Add as TSV
          </button>
          {deck.lang === "zh" && (
            <button
              onClick={handleAutofillAll}
              className="rounded border border-zinc-300 px-3 py-1 text-xs dark:border-zinc-700"
            >
              Autofill pinyin + gloss from CC-CEDICT
            </button>
          )}
        </div>
        {importWarnings.length > 0 && (
          <ul className="mt-2 text-xs text-amber-600">
            {importWarnings.map((w, i) => (
              <li key={i}>{w}</li>
            ))}
          </ul>
        )}
      </section>

      <section className="mt-6">
        <table className="w-full border-collapse text-sm">
          <thead>
            <tr className="border-b border-zinc-200 text-left text-xs text-zinc-500 dark:border-zinc-800">
              <th className="py-1 pr-2">Front</th>
              <th className="py-1 pr-2">Reading</th>
              <th className="py-1 pr-2">Back (gloss)</th>
              <th className="py-1" />
            </tr>
          </thead>
          <tbody>
            {cards.map((card) => (
              <tr key={card.localId} className="border-b border-zinc-100 dark:border-zinc-900">
                <td className="py-1 pr-2">
                  <input
                    className="w-full rounded border border-zinc-300 bg-white px-2 py-1 dark:border-zinc-700 dark:bg-zinc-900"
                    value={card.front}
                    onChange={(e) => updateCard(card.localId, { front: e.target.value })}
                  />
                </td>
                <td className="py-1 pr-2">
                  <input
                    className="w-full rounded border border-zinc-300 bg-white px-2 py-1 dark:border-zinc-700 dark:bg-zinc-900"
                    value={card.reading}
                    onChange={(e) => updateCard(card.localId, { reading: e.target.value })}
                  />
                </td>
                <td className="py-1 pr-2">
                  <input
                    className="w-full rounded border border-zinc-300 bg-white px-2 py-1 dark:border-zinc-700 dark:bg-zinc-900"
                    value={card.back}
                    onChange={(e) => updateCard(card.localId, { back: e.target.value })}
                  />
                </td>
                <td className="py-1">
                  <button
                    onClick={() => removeCard(card.localId)}
                    className="text-xs text-red-600"
                  >
                    Remove
                  </button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
        <button
          onClick={addBlankRow}
          className="mt-2 rounded border border-zinc-300 px-3 py-1 text-xs dark:border-zinc-700"
        >
          Add row
        </button>
      </section>

      <section className="mt-8 flex items-center gap-3">
        <button
          onClick={handleCompileDownload}
          className="rounded bg-zinc-900 px-4 py-1.5 text-sm font-medium text-white dark:bg-zinc-100 dark:text-zinc-900"
        >
          Compile and download .srs
        </button>
        {status && <span className="text-xs text-zinc-500">{status}</span>}
      </section>

      <section className="mt-10 rounded-lg border border-amber-300 p-4 dark:border-amber-900">
        <h2 className="text-sm font-semibold text-amber-700 dark:text-amber-400">Danger zone</h2>
        <p className="mt-1 text-xs text-zinc-500">
          Changing the slug orphans this deck&apos;s review history on the device: every card is
          re-derived from (slug, headword), so a new slug means new ids.
        </p>
        <div className="mt-2 flex gap-2">
          <input
            className="w-40 rounded border border-zinc-300 bg-white px-2 py-1 text-xs dark:border-zinc-700 dark:bg-zinc-900"
            placeholder="new-slug"
            value={newSlug}
            onChange={(e) => setNewSlug(e.target.value)}
          />
          <button
            onClick={handleChangeSlug}
            className="rounded border border-amber-400 px-3 py-1 text-xs text-amber-700 dark:text-amber-400"
          >
            Change slug
          </button>
        </div>
      </section>
    </div>
  );
}
