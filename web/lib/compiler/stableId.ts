// Card id derivation, mirroring tools/deckc.py's stable_id() exactly so the
// two compilers assign identical ids to identical (slug, headword) pairs.
// See docs/deck-format.md: id is the only thing joining content to
// scheduling state, so it must be stable across recompiles.

/**
 * 64-bit card id, stable across recompiles.
 *
 * Derived from the deck slug and the headword only, never from row order,
 * so reordering or inserting rows preserves scheduling state. Changing a
 * headword deliberately creates a new card; see docs/deck-format.md.
 *
 * Matches Python: int.from_bytes(sha256(f"{slug}\x00{headword}")[:8], "little") & 0x7FFFFFFFFFFFFFFF
 */
export async function stableId(deckSlug: string, headword: string): Promise<bigint> {
  const bytes = new TextEncoder().encode(`${deckSlug}\x00${headword}`);
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  let value = 0n;
  // Little-endian: byte 0 is the least-significant byte.
  for (let i = 7; i >= 0; i--) {
    value = (value << 8n) | BigInt(digest[i]);
  }
  return value & 0x7fffffffffffffffn;
}
