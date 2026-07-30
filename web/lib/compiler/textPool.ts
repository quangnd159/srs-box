// Deduplicating UTF-8 blob, mirroring tools/deckc.py's TextPool. Cards
// sharing a gloss (or any other string) share bytes, and insertion order is
// preserved so the byte layout matches the Python compiler exactly for
// identical input.

export class TextPool {
  private chunks: Uint8Array[] = [];
  private length = 0;
  private offsets = new Map<string, [number, number]>();

  /** Appends `s` if new, returns its [offset, length] into the pool. */
  add(s: string): [number, number] {
    if (!s) return [0, 0];
    const existing = this.offsets.get(s);
    if (existing) return existing;
    const raw = new TextEncoder().encode(s);
    if (raw.length > 0xffff) {
      throw new Error(`string too long for a u16 length: ${s.slice(0, 40)}`);
    }
    const entry: [number, number] = [this.length, raw.length];
    this.chunks.push(raw);
    this.length += raw.length;
    this.offsets.set(s, entry);
    return entry;
  }

  get byteLength(): number {
    return this.length;
  }

  get uniqueCount(): number {
    return this.offsets.size;
  }

  toBytes(): Uint8Array {
    const out = new Uint8Array(this.length);
    let pos = 0;
    for (const chunk of this.chunks) {
      out.set(chunk, pos);
      pos += chunk.length;
    }
    return out;
  }
}
