#include "home_ui.h"

#include <cstdio>

namespace home {
namespace {

// Same palette as review_ui.cpp's queue colours -- the whole point is that
// they're already in the user's head from the desktop app, so the picker
// should look like a continuation of the review screen, not a new idiom.
inline lv_color_t kBlue() { return lv_color_hex(0x3B82F6); }   // learning
inline lv_color_t kGreen() { return lv_color_hex(0x22C55E); }  // due
inline lv_color_t kAmber() { return lv_color_hex(0xF59E0B); }  // new
inline lv_color_t kBg() { return lv_color_hex(0x101014); }
inline lv_color_t kFg() { return lv_color_hex(0xF5F5F5); }
inline lv_color_t kMuted() { return lv_color_hex(0x9CA3AF); }
inline lv_color_t kRowBg() { return lv_color_hex(0x18181D); }

constexpr int kMaxRows = 12;  // generous; a handheld deck list won't approach this

struct Row {
  lv_obj_t* obj = nullptr;
  lv_obj_t* name = nullptr;
  lv_obj_t* fresh = nullptr;
  lv_obj_t* learning = nullptr;
  lv_obj_t* due = nullptr;
};

struct Widgets {
  lv_obj_t* root = nullptr;
  lv_obj_t* topbar = nullptr;
  lv_obj_t* clock = nullptr;
  lv_obj_t* battery = nullptr;
  lv_obj_t* list = nullptr;

  Row rows[kMaxRows];
  int row_count = 0;

  int w = 0;
  int h = 0;

  SelectCallback on_select = nullptr;
  RefreshCallback on_refresh = nullptr;
  lv_timer_t* refresh_timer = nullptr;

  // Deck names carry deck-language glyphs (the ç in "Français") that
  // Montserrat's ASCII set cannot draw, so rows use the deck-subset font.
  const lv_font_t* name_font = &lv_font_montserrat_14;
};

Widgets g;

lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t colour) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, colour, LV_PART_MAIN);
  lv_label_set_text(l, "");
  return l;
}

// Builds one deck row: name on the left, three colour-coded counts on the
// right, matching review_ui's top-bar count layout.
Row make_row(lv_obj_t* parent) {
  Row r;
  r.obj = lv_obj_create(parent);
  lv_obj_set_size(r.obj, lv_pct(100), 44);
  lv_obj_set_style_bg_color(r.obj, kRowBg(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(r.obj, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(r.obj, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(r.obj, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_all(r.obj, 8, LV_PART_MAIN);
  lv_obj_clear_flag(r.obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(r.obj, LV_OBJ_FLAG_CLICKABLE);

  r.name = make_label(r.obj, g.name_font, kFg());
  lv_obj_align(r.name, LV_ALIGN_LEFT_MID, 0, 0);

  r.fresh = make_label(r.obj, &lv_font_montserrat_14, kAmber());
  lv_obj_align(r.fresh, LV_ALIGN_RIGHT_MID, 0, 0);
  r.due = make_label(r.obj, &lv_font_montserrat_14, kGreen());
  lv_obj_align(r.due, LV_ALIGN_RIGHT_MID, -32, 0);
  r.learning = make_label(r.obj, &lv_font_montserrat_14, kBlue());
  lv_obj_align(r.learning, LV_ALIGN_RIGHT_MID, -64, 0);

  return r;
}

void refresh_timer_cb(lv_timer_t*) {
  if (g.on_refresh) g.on_refresh();
}

}  // namespace

void set_name_font(const lv_font_t* font) {
  if (font) g.name_font = font;
}

void init(int hor_res, int ver_res) {
  g.w = hor_res;
  g.h = ver_res;

  lv_obj_t* scr = lv_obj_create(nullptr);
  g.root = scr;
  lv_obj_set_style_bg_color(scr, kBg(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

  // --- status bar -----------------------------------------------------------
  g.topbar = lv_obj_create(scr);
  lv_obj_set_size(g.topbar, hor_res, 26);
  lv_obj_align(g.topbar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(g.topbar, lv_color_hex(0x18181D), LV_PART_MAIN);
  lv_obj_set_style_border_width(g.topbar, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(g.topbar, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(g.topbar, 0, LV_PART_MAIN);
  lv_obj_clear_flag(g.topbar, LV_OBJ_FLAG_SCROLLABLE);

  g.clock = make_label(g.topbar, &lv_font_montserrat_14, kMuted());
  lv_obj_align(g.clock, LV_ALIGN_LEFT_MID, 6, 0);

  g.battery = make_label(g.topbar, &lv_font_montserrat_14, kMuted());
  lv_obj_align(g.battery, LV_ALIGN_RIGHT_MID, -6, 0);

  // --- deck list --------------------------------------------------------------
  g.list = lv_obj_create(scr);
  lv_obj_set_size(g.list, hor_res, ver_res - 26);
  lv_obj_align(g.list, LV_ALIGN_TOP_MID, 0, 26);
  lv_obj_set_style_bg_opa(g.list, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(g.list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(g.list, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_row(g.list, 6, LV_PART_MAIN);
  lv_obj_set_flex_flow(g.list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(g.list, LV_DIR_VER);

  for (int i = 0; i < kMaxRows; ++i) {
    g.rows[i] = make_row(g.list);
    lv_obj_add_flag(g.rows[i].obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(
        g.rows[i].obj,
        [](lv_event_t* e) {
          const int index =
              static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
          if (g.on_select) g.on_select(index);
        },
        LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
  }

  // ~once a minute is enough for a clock display and a battery reading that
  // changes over minutes, not seconds; see home_ui.h's doc comment.
  g.refresh_timer = lv_timer_create(refresh_timer_cb, 60000, nullptr);

  lv_screen_load(scr);
}

void set_decks(const DeckRow* rows, int count) {
  if (count > kMaxRows) count = kMaxRows;
  g.row_count = count;
  for (int i = 0; i < kMaxRows; ++i) {
    if (i >= count) {
      lv_obj_add_flag(g.rows[i].obj, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(g.rows[i].obj, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(g.rows[i].name, rows[i].name);
    lv_label_set_text_fmt(g.rows[i].fresh, "%d", rows[i].fresh);
    lv_label_set_text_fmt(g.rows[i].due, "%d", rows[i].due);
    lv_label_set_text_fmt(g.rows[i].learning, "%d", rows[i].learning);
  }
}

void set_status(const Status& s) {
  if (s.hour < 0 || s.minute < 0) {
    lv_label_set_text(g.clock, "--:--");
  } else {
    lv_label_set_text_fmt(g.clock, "%02d:%02d", s.hour, s.minute);
  }

  if (s.battery_percent < 0) {
    lv_label_set_text(g.battery, "batt ?");
  } else {
    lv_label_set_text_fmt(g.battery, "%s%d%%", s.charging ? "+" : "", s.battery_percent);
  }
}

void set_select_callback(SelectCallback on_select) { g.on_select = on_select; }

void set_refresh_callback(RefreshCallback on_refresh) { g.on_refresh = on_refresh; }

lv_obj_t* screen() { return g.root; }

}  // namespace home
