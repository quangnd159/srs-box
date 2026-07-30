import { describe, expect, test } from "bun:test";
import {
  DeckNotFoundError,
  DeckStore,
  InvalidSlugError,
  MemoryStore,
  SlugTakenError,
} from "../lib/deck/store";

function makeStore(): DeckStore {
  return new DeckStore(new MemoryStore());
}

describe("DeckStore", () => {
  test("creates and lists decks", () => {
    const store = makeStore();
    store.createDeck({ name: "HSK 1", slug: "hsk1", lang: "zh" });
    store.createDeck({ name: "French basics", slug: "fr-basics", lang: "fr" });
    const decks = store.listDecks();
    expect(decks).toHaveLength(2);
    expect(decks.map((d) => d.slug).sort()).toEqual(["fr-basics", "hsk1"]);
  });

  test("rejects a duplicate slug", () => {
    const store = makeStore();
    store.createDeck({ name: "HSK 1", slug: "hsk1", lang: "zh" });
    expect(() => store.createDeck({ name: "Other", slug: "hsk1", lang: "zh" })).toThrow(
      SlugTakenError,
    );
  });

  test("rejects an invalid slug", () => {
    const store = makeStore();
    expect(() => store.createDeck({ name: "Bad", slug: "Has Spaces", lang: "zh" })).toThrow(
      InvalidSlugError,
    );
  });

  test("new decks start with an empty card list", () => {
    const store = makeStore();
    store.createDeck({ name: "HSK 1", slug: "hsk1", lang: "zh" });
    expect(store.getCards("hsk1")).toEqual([]);
  });

  test("renaming preserves slug and cards", () => {
    const store = makeStore();
    store.createDeck({ name: "HSK 1", slug: "hsk1", lang: "zh" });
    store.setCards("hsk1", [{ localId: "1", front: "爱", reading: "ài", back: "love" }]);
    store.renameDeck("hsk1", "HSK Level 1");
    expect(store.getDeck("hsk1")?.name).toBe("HSK Level 1");
    expect(store.getCards("hsk1")).toHaveLength(1);
  });

  test("changeSlug moves cards to the new key and drops the old one", () => {
    const store = makeStore();
    store.createDeck({ name: "HSK 1", slug: "hsk1", lang: "zh" });
    store.setCards("hsk1", [{ localId: "1", front: "爱", reading: "ài", back: "love" }]);
    store.changeSlug("hsk1", "hsk-1-v2");
    expect(store.getDeck("hsk1")).toBeUndefined();
    expect(store.getDeck("hsk-1-v2")).toBeDefined();
    expect(store.getCards("hsk-1-v2")).toHaveLength(1);
    expect(store.getCards("hsk1")).toEqual([]);
  });

  test("changeSlug refuses to collide with an existing deck", () => {
    const store = makeStore();
    store.createDeck({ name: "HSK 1", slug: "hsk1", lang: "zh" });
    store.createDeck({ name: "HSK 2", slug: "hsk2", lang: "zh" });
    expect(() => store.changeSlug("hsk1", "hsk2")).toThrow(SlugTakenError);
  });

  test("deleteDeck removes the deck and its cards", () => {
    const store = makeStore();
    store.createDeck({ name: "HSK 1", slug: "hsk1", lang: "zh" });
    store.deleteDeck("hsk1");
    expect(store.listDecks()).toEqual([]);
    expect(store.getCards("hsk1")).toEqual([]);
  });

  test("setCards on a nonexistent deck throws", () => {
    const store = makeStore();
    expect(() => store.setCards("ghost", [])).toThrow(DeckNotFoundError);
  });

  test("addCards appends to the existing list", () => {
    const store = makeStore();
    store.createDeck({ name: "HSK 1", slug: "hsk1", lang: "zh" });
    store.addCards("hsk1", [{ localId: "1", front: "爱", reading: "ài", back: "love" }]);
    store.addCards("hsk1", [{ localId: "2", front: "八", reading: "bā", back: "eight" }]);
    expect(store.getCards("hsk1")).toHaveLength(2);
  });
});
