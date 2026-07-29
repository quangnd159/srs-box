#!/usr/bin/env bash
# Subset a CJK font down to exactly the glyphs a deck uses.
#
#   tools/buildfont.sh decks/hsk1-2.glyphs.txt
#
# Shipping all of Unicode CJK would be megabytes; shipping only the characters
# present in the deck is a rounding error. Re-run whenever the deck changes,
# or cards will render as blank boxes.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GLYPHS="${1:-$ROOT/decks/hsk1-2.glyphs.txt}"
FONT="${FONT:-$ROOT/assets/fonts/NotoSansSC-Regular.ttf}"
OUTDIR="${OUTDIR:-$ROOT/firmware/main/fonts}"

# UI glyphs the deck itself never contains. Without these, anything the
# firmware draws that is not deck text renders as a tofu box: U+2713 check
# mark for the finished screen, U+00B7 middle dot as a separator.
#
# Any future UI character must be added here. A missing glyph fails silently
# as an empty rectangle rather than as an error.

# Headword size and UI/gloss size. All are cheap; see docs/deck-format.md.
SIZES="${SIZES:-16 20 28 48}"

[[ -f "$GLYPHS" ]] || { echo "no glyph list at $GLYPHS (run tools/deckc.py --glyphs)" >&2; exit 1; }
[[ -f "$FONT" ]] || { echo "no font at $FONT" >&2; exit 1; }

mkdir -p "$OUTDIR"
symbols="$(cat "$GLYPHS")"

for sz in $SIZES; do
  out="$OUTDIR/font_cjk_${sz}.c"
  npx lv_font_conv \
    --font "$FONT" \
    --size "$sz" \
    --bpp 4 \
    --format lvgl \
    --no-compress \
    --lv-include lvgl.h \
    --symbols "$symbols" \
    -r 0x20-0x7F \
    -r 0x2713 \
    -r 0x00B7 \
    -o "$out"
  bytes=$(grep -o "0x[0-9a-fA-F][0-9a-fA-F]" "$out" | wc -l | tr -d ' ')
  echo "  ${sz}px -> $out  (~$((bytes / 1024)) KB binary)"
done
