// Deck manager persistence. Decks live in localStorage (this is a
// single-user tool, not a synced database); the store takes a Storage-like
// interface so it's unit-testable without a browser (see
// test/deckStore.test.ts for an in-memory fake).
import { Card, Deck, isValidSlug } from "./types";

/** Minimal subset of the DOM Storage interface this module needs. */
export interface KeyValueStore {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
  removeItem(key: string): void;
}

const INDEX_KEY = "srsbox.decks.index";
const cardsKey = (slug: string) => `srsbox.decks.cards.${slug}`;

export class SlugTakenError extends Error {
  constructor(slug: string) {
    super(`a deck with slug "${slug}" already exists`);
  }
}

export class InvalidSlugError extends Error {
  constructor(slug: string) {
    super(`"${slug}" is not a valid slug: use lowercase letters, digits, and hyphens`);
  }
}

export class DeckNotFoundError extends Error {
  constructor(slug: string) {
    super(`no deck with slug "${slug}"`);
  }
}

export class DeckStore {
  constructor(private readonly kv: KeyValueStore) {}

  listDecks(): Deck[] {
    const raw = this.kv.getItem(INDEX_KEY);
    if (!raw) return [];
    return (JSON.parse(raw) as Deck[]).sort((a, b) => a.name.localeCompare(b.name));
  }

  getDeck(slug: string): Deck | undefined {
    return this.listDecks().find((d) => d.slug === slug);
  }

  private saveIndex(decks: Deck[]): void {
    this.kv.setItem(INDEX_KEY, JSON.stringify(decks));
  }

  createDeck(input: { name: string; slug: string; lang: string }): Deck {
    if (!isValidSlug(input.slug)) throw new InvalidSlugError(input.slug);
    const decks = this.listDecks();
    if (decks.some((d) => d.slug === input.slug)) throw new SlugTakenError(input.slug);

    const now = new Date().toISOString();
    const deck: Deck = { slug: input.slug, name: input.name, lang: input.lang, createdAt: now, updatedAt: now };
    this.saveIndex([...decks, deck]);
    this.kv.setItem(cardsKey(deck.slug), JSON.stringify([]));
    return deck;
  }

  renameDeck(slug: string, name: string): Deck {
    const decks = this.listDecks();
    const deck = decks.find((d) => d.slug === slug);
    if (!deck) throw new DeckNotFoundError(slug);
    deck.name = name;
    deck.updatedAt = new Date().toISOString();
    this.saveIndex(decks);
    return deck;
  }

  /**
   * Changes a deck's slug. This orphans all existing scheduling state on
   * the device: card ids are derived from (slug, headword), so every card
   * gets a new id and its review history stops joining up. Callers must
   * warn the user before calling this; see docs/deck-format.md.
   */
  changeSlug(oldSlug: string, newSlug: string): Deck {
    if (!isValidSlug(newSlug)) throw new InvalidSlugError(newSlug);
    const decks = this.listDecks();
    const deck = decks.find((d) => d.slug === oldSlug);
    if (!deck) throw new DeckNotFoundError(oldSlug);
    if (newSlug !== oldSlug && decks.some((d) => d.slug === newSlug)) {
      throw new SlugTakenError(newSlug);
    }

    const cards = this.getCards(oldSlug);
    deck.slug = newSlug;
    deck.updatedAt = new Date().toISOString();
    this.saveIndex(decks);
    this.kv.setItem(cardsKey(newSlug), JSON.stringify(cards));
    if (newSlug !== oldSlug) this.kv.removeItem(cardsKey(oldSlug));
    return deck;
  }

  deleteDeck(slug: string): void {
    const decks = this.listDecks().filter((d) => d.slug !== slug);
    this.saveIndex(decks);
    this.kv.removeItem(cardsKey(slug));
  }

  getCards(slug: string): Card[] {
    const raw = this.kv.getItem(cardsKey(slug));
    return raw ? (JSON.parse(raw) as Card[]) : [];
  }

  setCards(slug: string, cards: Card[]): void {
    if (!this.getDeck(slug)) throw new DeckNotFoundError(slug);
    this.kv.setItem(cardsKey(slug), JSON.stringify(cards));
    const decks = this.listDecks();
    const deck = decks.find((d) => d.slug === slug);
    if (deck) {
      deck.updatedAt = new Date().toISOString();
      this.saveIndex(decks);
    }
  }

  addCards(slug: string, newCards: Card[]): Card[] {
    const merged = [...this.getCards(slug), ...newCards];
    this.setCards(slug, merged);
    return merged;
  }
}

/** In-memory KeyValueStore, for tests and any non-browser context. */
export class MemoryStore implements KeyValueStore {
  private data = new Map<string, string>();
  getItem(key: string): string | null {
    return this.data.has(key) ? this.data.get(key)! : null;
  }
  setItem(key: string, value: string): void {
    this.data.set(key, value);
  }
  removeItem(key: string): void {
    this.data.delete(key);
  }
}

/** Browser localStorage, guarded so this module can be imported server-side. */
export function browserStore(): KeyValueStore {
  if (typeof window === "undefined") {
    throw new Error("browserStore() called outside the browser");
  }
  return window.localStorage;
}
