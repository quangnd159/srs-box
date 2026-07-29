#!/usr/bin/env bash
# Build and run the headless UI simulator, then convert frames to PNG.
#
#   sim/run.sh [width height]
#
# No hardware, no window, no SDL. Renders the review screen into a memory
# buffer and writes PNGs, so the layout can be iterated in milliseconds
# instead of a flash-and-squint cycle. The UI code is shared verbatim with
# the firmware, so what renders here is what the panel shows.
set -euo pipefail

# cmake and ninja come from the ESP-IDF tool install, not the system.
CMAKE_BIN="$(ls -d "$HOME"/.espressif/tools/cmake/*/CMake.app/Contents/bin 2>/dev/null | head -1)"
NINJA_BIN="$(ls -d "$HOME"/.espressif/tools/ninja/* 2>/dev/null | head -1)"
[[ -n "$CMAKE_BIN" ]] && export PATH="$CMAKE_BIN:$PATH"
[[ -n "$NINJA_BIN" ]] && export PATH="$NINJA_BIN:$PATH"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$HERE/.."
W="${1:-240}"
H="${2:-320}"

cmake -S "$HERE" -B "$HERE/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$HERE/build" -j8 >/dev/null

mkdir -p "$HERE/out"
rm -f "$HERE/out"/*.raw "$HERE/out"/*.png
"$HERE/build/srs_sim" --size "$W" "$H" --out "$HERE/out"

"$ROOT/.venv/bin/python" - "$HERE/out" "$W" "$H" <<'PY'
import sys, pathlib
from PIL import Image
outdir, w, h = pathlib.Path(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
for raw in sorted(outdir.glob("*.raw")):
    img = Image.frombytes("RGB", (w, h), raw.read_bytes(), "raw", "BGR;16")
    png = raw.with_suffix(".png")
    img.save(png)
    raw.unlink()
    print(f"  {png.name}")
PY
