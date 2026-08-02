#!/usr/bin/env bash
# Subset fonts down to exactly the glyphs the installed decks use.
#
#   tools/buildfont.sh decks/hsk1-2.glyphs.txt [more.glyphs.txt ...]
#
# Shipping all of Unicode would be megabytes; shipping only the characters
# present in the decks is a rounding error. Re-run whenever a deck changes,
# or cards will render as blank boxes.
#
# Two font sources, because no single Noto face covers everything the decks
# need: Noto Sans SC has the hanzi but NO IPA (french-a1's readings would
# silently render blank), while plain Noto Sans has IPA, Vietnamese, and the
# undertie but no CJK. lv_font_conv merges ranges from multiple --font args.
#
#   FORMAT=bin OUTDIR=... tools/buildfont.sh ...   emits LVGL binary fonts
#   (font_cjk_<size>.bin) for pushing to /data/fonts/ instead of compiled-in
#   C arrays.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CJK_FONT="${CJK_FONT:-$ROOT/assets/fonts/NotoSansSC-Regular.ttf}"
LATIN_FONT="${LATIN_FONT:-$ROOT/assets/fonts/NotoSans-Regular.ttf}"
FORMAT="${FORMAT:-lvgl}"
OUTDIR="${OUTDIR:-$ROOT/firmware/main/fonts}"

# Headword size and UI/gloss sizes. All are cheap; see docs/deck-format.md.
SIZES="${SIZES:-16 20 28 48}"

[[ $# -ge 1 ]] || set -- "$ROOT/decks/hsk1-2.glyphs.txt"
for g in "$@"; do
  [[ -f "$g" ]] || { echo "no glyph list at $g (run tools/deckc.py --glyphs)" >&2; exit 1; }
done
[[ -f "$CJK_FONT" ]] || { echo "no font at $CJK_FONT" >&2; exit 1; }
[[ -f "$LATIN_FONT" ]] || { echo "no font at $LATIN_FONT" >&2; exit 1; }

# Union of every given glyph list, split by which font must supply it.
# The CJK cut-off (U+2E80) is the start of the CJK-adjacent blocks; nothing
# a deck uses between IPA/Vietnamese Latin and there is in Noto Sans SC.
split="$(python3 - "$@" <<'EOF'
import sys
glyphs = set()
for path in sys.argv[1:]:
    glyphs |= set(open(path, encoding="utf-8").read())
# Control characters have no glyph: the compiler's syllable separator
# (U+001F, see tools/deckc.py) must never be requested here.
glyphs = {c for c in glyphs if c > " "}  # drops whitespace and control chars
cjk = "".join(sorted(c for c in glyphs if ord(c) >= 0x2E80))
latin = "".join(sorted(c for c in glyphs if ord(c) < 0x2E80))
print(latin)
print(cjk)
EOF
)"
latin_symbols="$(sed -n 1p <<<"$split")"
cjk_symbols="$(sed -n 2p <<<"$split")"

mkdir -p "$OUTDIR"

for sz in $SIZES; do
  if [[ "$FORMAT" == "bin" ]]; then out="$OUTDIR/font_cjk_${sz}.bin"; else out="$OUTDIR/font_cjk_${sz}.c"; fi
  args=(
    --size "$sz" --bpp 4 --format "$FORMAT" --no-compress -o "$out"
    # UI glyphs the decks themselves may not contain. Without these,
    # anything the firmware draws that is not deck text renders as a tofu
    # box: U+2713 check mark for the finished screen (from the SC face,
    # which has it; plain Noto Sans does not), U+00B7 separator dot.
    # Any future UI character must be added here — a missing glyph fails
    # silently as an empty rectangle rather than as an error.
    --font "$LATIN_FONT" -r 0x20-0x7F -r 0x00B7
  )
  [[ -n "$latin_symbols" ]] && args+=(--symbols "$latin_symbols")
  args+=(--font "$CJK_FONT" -r 0x2713)
  [[ -n "$cjk_symbols" ]] && args+=(--symbols "$cjk_symbols")
  [[ "$FORMAT" == "lvgl" ]] && args+=(--lv-include lvgl.h)
  npx lv_font_conv "${args[@]}"
  echo "  ${sz}px -> $out  ($(du -h "$out" | cut -f1))"
done
