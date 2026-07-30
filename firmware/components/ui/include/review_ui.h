// The review screen.
//
// Pure LVGL with no ESP-IDF dependency, so the host simulator in sim/ and the
// device firmware build the identical widget tree. Iterate the layout in the
// simulator, where a render costs milliseconds and produces a PNG, then flash.

#pragma once

#include <lvgl.h>

namespace ui {

struct CardView {
  const char* front = "";    // headword, e.g. hanzi
  const char* reading = "";  // pinyin; may be empty
  const char* back = "";     // gloss
};

struct Counts {
  int learning = 0;
  int due = 0;
  int fresh = 0;  // unseen cards
};

// Interval labels shown on the grading buttons, pre-formatted by the caller
// ("10m", "1d", "3d"). Anki shows these and they teach honest grading.
struct Intervals {
  const char* again = "";
  const char* hard = "";
  const char* good = "";
  const char* easy = "";
};

// Builds the widget tree on the active screen. Call once.
void init(int hor_res, int ver_res);

void set_deck_name(const char* name);
void set_counts(const Counts& c);
void set_battery(int percent, bool charging);

// `revealed` false shows only the headword (the recall prompt); true reveals
// the reading, gloss, and grading buttons.
void show_card(const CardView& card, bool revealed);

void set_intervals(const Intervals& iv);

// Shown when nothing is due.
void show_done(int reviewed_today);

// Input. Tapping anywhere in the card body reveals the answer; tapping a
// grade button reports a rating of 1..4 (Again, Hard, Good, Easy).
//
// Grades fire on release with no double-click window. Double-click detection
// would add latency to every single card, which is felt immediately in a
// review loop. See CLAUDE.md.
using RevealCallback = void (*)();
using GradeCallback = void (*)(int rating);
void set_callbacks(RevealCallback on_reveal, GradeCallback on_grade);

// Small "<-" tap target in the top-left corner, for returning to the deck
// picker (home screen). Deliberately touch-only: the physical buttons keep
// their existing meaning exactly as before (minus/plus grade, any-button
// reveals), and the power button must stay inert here beyond its existing
// short-press role -- see docs/pinout.md.
using BackCallback = void (*)();
void set_back_callback(BackCallback on_back);

// Which language the open deck is in, e.g. "zh" or "fr". Gates pinyin tone
// colouring of the reading line (docs/sync-protocol.md): only lang "zh" (or
// absent, which parse_meta() defaults to "zh") gets per-syllable tone
// colours; every other language renders the reading in one neutral colour.
// Call before show_card(); "zh" is the default, so a caller that never calls
// this (e.g. the simulator's fixed HSK 1-2 scenes) gets tone colouring for
// free rather than silently losing it.
void set_lang(const char* lang);

// Runtime CJK fonts pushed to /data/fonts/ (docs/sync-protocol.md) replace
// the compiled-in subset when present. Pass nullptr for any size to keep the
// compiled-in font for that size -- this is also what happens before this is
// ever called, so it's safe to skip entirely when no runtime fonts loaded.
void set_fonts(const lv_font_t* f48, const lv_font_t* f28, const lv_font_t* f20,
               const lv_font_t* f16);

// Returns the LVGL screen object review_ui builds into, for switching
// between it and the home screen with lv_screen_load(). Valid after init().
lv_obj_t* screen();

}  // namespace ui
