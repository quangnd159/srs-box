// IEEE CRC-32 (the zlib/gzip polynomial), matching Python's zlib.crc32.
// Table-driven, standard implementation; not part of any external
// dependency so the compiler stays a pure module with no DOM/Node needs.

let table: Uint32Array | null = null;

function getTable(): Uint32Array {
  if (table) return table;
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    t[n] = c >>> 0;
  }
  table = t;
  return t;
}

/** IEEE CRC-32 over `data`, returned as an unsigned 32-bit integer. */
export function crc32(data: Uint8Array): number {
  const t = getTable();
  let crc = 0xffffffff;
  for (let i = 0; i < data.length; i++) {
    crc = t[(crc ^ data[i]) & 0xff] ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}
