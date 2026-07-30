"use client";

import { useMemo } from "react";
import { browserStore, DeckStore } from "./store";

/** One DeckStore instance per component tree, backed by window.localStorage. */
export function useDeckStore(): DeckStore {
  return useMemo(() => new DeckStore(browserStore()), []);
}
