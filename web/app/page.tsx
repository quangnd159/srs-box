"use client";

import Link from "next/link";
import { useState } from "react";
import { isValidSlug, type Deck } from "@/lib/deck/types";
import { useDeckStore } from "@/lib/deck/useDeckStore";

function slugify(name: string): string {
  return name
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "");
}

export default function DeckManagerPage() {
  const store = useDeckStore();
  const [decks, setDecks] = useState<Deck[]>(() => store.listDecks());
  const [name, setName] = useState("");
  const [slug, setSlug] = useState("");
  const [slugEdited, setSlugEdited] = useState(false);
  const [lang, setLang] = useState("zh");
  const [error, setError] = useState<string | null>(null);
  const [renaming, setRenaming] = useState<Record<string, string>>({});

  function refresh() {
    setDecks(store.listDecks());
  }

  function handleNameChange(value: string) {
    setName(value);
    if (!slugEdited) setSlug(slugify(value));
  }

  function handleCreate(e: React.FormEvent) {
    e.preventDefault();
    setError(null);
    try {
      store.createDeck({ name: name.trim(), slug, lang });
      setName("");
      setSlug("");
      setSlugEdited(false);
      refresh();
    } catch (err) {
      setError((err as Error).message);
    }
  }

  function handleDelete(deckSlug: string) {
    if (!confirm(`Delete deck "${deckSlug}"? This cannot be undone locally.`)) return;
    store.deleteDeck(deckSlug);
    refresh();
  }

  function handleRename(deckSlug: string) {
    const newName = renaming[deckSlug];
    if (!newName || !newName.trim()) return;
    store.renameDeck(deckSlug, newName.trim());
    setRenaming((r) => ({ ...r, [deckSlug]: "" }));
    refresh();
  }

  return (
    <div className="mx-auto max-w-3xl px-6 py-10">
      <h1 className="text-2xl font-semibold">Decks</h1>
      <p className="mt-1 text-sm text-zinc-600 dark:text-zinc-400">
        Input is a word list. The compiler turns it into cards, a font subset, and a
        pushable bundle for the device.
      </p>

      <form onSubmit={handleCreate} className="mt-8 flex flex-wrap items-end gap-3 rounded-lg border border-zinc-200 p-4 dark:border-zinc-800">
        <div className="flex flex-col gap-1">
          <label className="text-xs font-medium text-zinc-500">Name</label>
          <input
            className="w-48 rounded border border-zinc-300 bg-white px-2 py-1 text-sm dark:border-zinc-700 dark:bg-zinc-900"
            value={name}
            onChange={(e) => handleNameChange(e.target.value)}
            placeholder="HSK 1"
            required
          />
        </div>
        <div className="flex flex-col gap-1">
          <label className="text-xs font-medium text-zinc-500">Slug</label>
          <input
            className="w-36 rounded border border-zinc-300 bg-white px-2 py-1 text-sm dark:border-zinc-700 dark:bg-zinc-900"
            value={slug}
            onChange={(e) => {
              setSlug(e.target.value);
              setSlugEdited(true);
            }}
            placeholder="hsk1"
            required
          />
        </div>
        <div className="flex flex-col gap-1">
          <label className="text-xs font-medium text-zinc-500">Language</label>
          <select
            className="rounded border border-zinc-300 bg-white px-2 py-1 text-sm dark:border-zinc-700 dark:bg-zinc-900"
            value={lang}
            onChange={(e) => setLang(e.target.value)}
          >
            <option value="zh">Chinese (zh)</option>
            <option value="fr">French (fr)</option>
            <option value="other">Other</option>
          </select>
        </div>
        <button
          type="submit"
          className="rounded bg-zinc-900 px-4 py-1.5 text-sm font-medium text-white dark:bg-zinc-100 dark:text-zinc-900"
        >
          Create deck
        </button>
        {!isValidSlug(slug) && slug.length > 0 && (
          <span className="text-xs text-red-600">
            Slug must be lowercase letters, digits, and hyphens.
          </span>
        )}
      </form>
      {error && <p className="mt-2 text-sm text-red-600">{error}</p>}

      <ul className="mt-8 flex flex-col gap-3">
        {decks.length === 0 && (
          <li className="text-sm text-zinc-500">No decks yet. Create one above.</li>
        )}
        {decks.map((deck) => (
          <li
            key={deck.slug}
            className="flex items-center justify-between gap-4 rounded-lg border border-zinc-200 p-4 dark:border-zinc-800"
          >
            <div>
              <Link href={`/decks/${deck.slug}`} className="font-medium hover:underline">
                {deck.name}
              </Link>
              <p className="text-xs text-zinc-500">
                slug: {deck.slug} · lang: {deck.lang}
              </p>
            </div>
            <div className="flex items-center gap-2">
              <input
                className="w-32 rounded border border-zinc-300 bg-white px-2 py-1 text-xs dark:border-zinc-700 dark:bg-zinc-900"
                placeholder="rename..."
                value={renaming[deck.slug] ?? ""}
                onChange={(e) => setRenaming((r) => ({ ...r, [deck.slug]: e.target.value }))}
              />
              <button
                onClick={() => handleRename(deck.slug)}
                className="rounded border border-zinc-300 px-2 py-1 text-xs dark:border-zinc-700"
              >
                Rename
              </button>
              <button
                onClick={() => handleDelete(deck.slug)}
                className="rounded border border-red-300 px-2 py-1 text-xs text-red-600 dark:border-red-900"
              >
                Delete
              </button>
            </div>
          </li>
        ))}
      </ul>
    </div>
  );
}
