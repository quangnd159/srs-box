// The deck picker / home screen.
//
// Pure LVGL, no ESP-IDF dependency, same "build in the simulator, flash once
// it looks right" workflow as review_ui.h. Shown at boot and whenever a
// review session ends or the user taps review_ui's back arrow.

#pragma once

#include <lvgl.h>

namespace home {

// One row in the deck list.
struct DeckRow {
  const char* name = "";
  int fresh = 0;
  int learning = 0;
  int due = 0;
};

// Battery: percent < 0 means unknown (see components/power/), and is shown
// as "?" rather than any guessed number.
struct Status {
  int hour = -1;    // local hour, 0..23; < 0 means "clock unset", shows "--:--"
  int minute = -1;
  int battery_percent = -1;
  bool charging = false;
};

// Builds the widget tree on its own screen (see screen() below). Call once.
void init(int hor_res, int ver_res);

// Font for the deck-name labels; must cover the decks' own glyphs (see the
// comment in home_ui.cpp). Call before set_decks(); Montserrat otherwise.
void set_name_font(const lv_font_t* font);

// Replaces the deck list wholesale; cheap enough to call on every refresh
// (see set_refresh_callback()) since a study deck's row count is small.
void set_decks(const DeckRow* rows, int count);

void set_status(const Status& s);

using SelectCallback = void (*)(int index);
void set_select_callback(SelectCallback on_select);

// Runs ~once a minute (an internal lv_timer, not the caller's responsibility
// to schedule) so the clock and battery stay current even if nobody touches
// a deck row for a while. The callback is expected to turn around and call
// set_status()/set_decks() with fresh values -- home_ui itself knows nothing
// about the clock, the deck files, or power::battery_percent().
using RefreshCallback = void (*)();
void set_refresh_callback(RefreshCallback on_refresh);

// The LVGL screen object, for lv_screen_load(). Valid after init().
lv_obj_t* screen();

}  // namespace home
