// Lazy-loading loader for LVGL binary fonts (lv_font_conv --format bin).
//
// Drop-in replacement for lv_binfont_create() that does NOT read the glyph
// bitmaps at open time. lv_binfont_create() pulls the whole `glyf` section
// into RAM, which for the deck CJK subsets is megabytes off LittleFS and
// cost ~10s of boot. rtfont_create() reads only the small tables (head,
// cmap, loca, kern) and then faults individual glyphs in from the file the
// first time LVGL asks to draw them, caching them in PSRAM forever.
//
// The returned lv_font_t is byte-for-byte equivalent in layout terms to what
// lv_binfont_create() would have produced: same line_height, base_line,
// metrics and bitmaps, so nothing downstream needs to know the difference.
//
// The file format is untouched (see tools/buildfont.sh). LVGL's own parser,
// src/font/binfont_loader/lv_binfont_loader.c, is the reference this mirrors.

#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// `path` is a plain POSIX path ("/data/fonts/font_cjk_48.bin"), not an
// LVGL "A:"-prefixed one: this reads the file with stdio, not lv_fs.
// Returns NULL if the file is missing or malformed.
lv_font_t* rtfont_create(const char* path);

void rtfont_destroy(lv_font_t* font);

// Reads every not-yet-cached glyph, in file order, in large sequential
// chunks. Once this returns the font never touches the filesystem again.
// Safe to run from a background task while LVGL draws with the same font.
// Returns false if a read failed partway (the font stays usable).
bool rtfont_prewarm(lv_font_t* font);

#ifdef __cplusplus
}
#endif
