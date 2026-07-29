#!/usr/bin/env bash
# Chunked flash dump with retries. Slow but survives the serial dropouts
# this board shows on long single-shot reads.
set -u
PORT="${PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
OUT="${1:-backup/stock-parts}"
ESPTOOL="${ESPTOOL:-$(dirname "$0")/../.venv/bin/esptool}"
mkdir -p "$OUT"
CHUNK=$((512*1024))
N=$((16*1024*1024/CHUNK))
for ((i=0;i<N;i++)); do
  off=$((i*CHUNK))
  f=$(printf "%s/p%03d.bin" "$OUT" $i)
  if [ -s "$f" ] && [ "$(stat -f%z "$f")" -eq "$CHUNK" ]; then continue; fi
  for try in 1 2 3 4; do
    if "$ESPTOOL" --port "$PORT" --baud 230400 read-flash $off $CHUNK "$f" >/dev/null 2>&1; then
      echo "ok $(printf 0x%06x $off)"; break
    fi
    rm -f "$f"; sleep 2
  done
done
# Verify every chunk is present and full size before merging. A partial dump
# that happens to total 16MB is worse than no dump: it looks like a valid
# backup and restores garbage. This has bitten once already.
missing=()
for ((i=0;i<N;i++)); do
  f=$(printf "%s/p%03d.bin" "$OUT" $i)
  if [ ! -s "$f" ] || [ "$(stat -f%z "$f")" -ne "$CHUNK" ]; then
    missing+=("$i")
  fi
done
if [ ${#missing[@]} -ne 0 ]; then
  echo "INCOMPLETE: ${#missing[@]} chunk(s) missing or short: ${missing[*]}" >&2
  echo "Re-run this script; completed chunks are skipped." >&2
  exit 1
fi

# Explicit index order, never a glob: a bare p*.bin also matches leftovers
# like part_00.bin and splices them in at the wrong offset.
: > backup/stock-xiaozhi-16mb.bin
for ((i=0;i<N;i++)); do
  cat "$(printf "%s/p%03d.bin" "$OUT" $i)" >> backup/stock-xiaozhi-16mb.bin
done

size=$(stat -f%z backup/stock-xiaozhi-16mb.bin)
if [ "$size" -ne $((16*1024*1024)) ]; then
  echo "BAD SIZE: got $size, expected $((16*1024*1024))" >&2
  exit 1
fi
echo "backup complete and verified: backup/stock-xiaozhi-16mb.bin ($size bytes)"
