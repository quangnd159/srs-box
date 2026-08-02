"use client";

import { useMemo, useSyncExternalStore } from "react";
import { browserStore, DeckStore, MemoryStore } from "./store";

const noopSubscribe = () => () => {};

/**
 * True once this component has hydrated on the client. The server snapshot
 * and React's first client render (hydration, which must match the server's
 * output) both report `false`; the snapshot flips to `true` right after
 * mount. This is the standard useSyncExternalStore "is this the client yet"
 * pattern, and avoids sprinkling `typeof window` checks through consumers.
 */
export function useHasMounted(): boolean {
  return useSyncExternalStore(noopSubscribe, () => true, () => false);
}

/**
 * One DeckStore instance per component tree. localStorage can't be touched
 * during server rendering, nor during hydration's first client render, so
 * until this component has mounted the store is backed by an always-empty
 * in-memory stand-in instead of throwing. Once mounted it swaps to
 * window.localStorage, which changes the returned instance's identity;
 * consumers that cached data from the store in state (rather than reading
 * it fresh every render) should re-read it in a `useEffect` keyed on the
 * store, so they pick up the real persisted data after that swap.
 */
export function useDeckStore(): DeckStore {
  const hasMounted = useHasMounted();
  return useMemo(() => new DeckStore(hasMounted ? browserStore() : new MemoryStore()), [hasMounted]);
}
