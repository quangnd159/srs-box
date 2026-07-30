// Headless LVGL simulator.
//
// Renders the review screen into a memory buffer and writes raw RGB565
// frames, which sim/run.sh converts to PNG. No SDL and no window, so it runs
// anywhere and the output can be inspected directly.
//
// The point is to iterate the UI without hardware: a render here costs
// milliseconds, versus flashing a board and squinting at a 2" panel.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <lvgl.h>

#include "deck.h"
#include "deck_registry.h"
#include "fsrs.h"
#include "home_ui.h"
#include "review_ui.h"
#include "session.h"

namespace {

int g_hor = 240;
int g_ver = 320;
std::vector<uint16_t> g_framebuffer;
std::string g_outdir = "out";
// Real, compiled decks under decks/ (see tools/deckc.py) -- not a fixture
// invented for this test. hsk1-2.srs is lang=zh, french-a1.srs is lang=fr,
// which is exactly the pairing needed to exercise both the multi-deck picker
// and the lang-gated tone colouring (docs/sync-protocol.md) in one pass.
std::string g_decks_dir = "decks";

// LVGL hands us rendered tiles; copy them into the full framebuffer.
void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  auto* src = reinterpret_cast<uint16_t*>(px_map);
  for (int y = area->y1; y <= area->y2; ++y) {
    for (int x = area->x1; x <= area->x2; ++x) {
      if (x >= 0 && x < g_hor && y >= 0 && y < g_ver) {
        g_framebuffer[static_cast<size_t>(y) * g_hor + x] =
            src[(y - area->y1) * (area->x2 - area->x1 + 1) + (x - area->x1)];
      }
    }
  }
  lv_display_flush_ready(disp);
}

uint32_t g_tick = 0;
uint32_t tick_cb() { return g_tick; }

// Let LVGL settle: it renders lazily through timers.
void pump(int ms = 60) {
  for (int i = 0; i < ms; ++i) {
    g_tick += 1;
    lv_timer_handler();
  }
}

void save(const std::string& name) {
  pump();
  const std::string path = g_outdir + "/" + name + ".raw";
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    std::printf("cannot write %s\n", path.c_str());
    return;
  }
  std::fwrite(g_framebuffer.data(), sizeof(uint16_t), g_framebuffer.size(), f);
  std::fclose(f);
  std::printf("  %s (%dx%d)\n", path.c_str(), g_hor, g_ver);
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--size") == 0 && i + 2 < argc) {
      g_hor = std::atoi(argv[++i]);
      g_ver = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      g_outdir = argv[++i];
    } else if (std::strcmp(argv[i], "--decks-dir") == 0 && i + 1 < argc) {
      g_decks_dir = argv[++i];
    }
  }

  g_framebuffer.assign(static_cast<size_t>(g_hor) * g_ver, 0);

  lv_init();
  lv_tick_set_cb(tick_cb);

  lv_display_t* disp = lv_display_create(g_hor, g_ver);
  static std::vector<uint8_t> draw_buf;
  draw_buf.resize(static_cast<size_t>(g_hor) * g_ver * 2);
  lv_display_set_buffers(disp, draw_buf.data(), nullptr, draw_buf.size(),
                         LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(disp, flush_cb);

  std::printf("rendering %dx%d\n", g_hor, g_ver);
  ui::init(g_hor, g_ver);
  ui::set_deck_name("HSK 1-2");
  ui::set_counts({3, 12, 15});

  // Scene 1: the prompt. Only the hanzi, nothing to read ahead to.
  ui::show_card({"咖啡", "kāfēi", "cà phê"}, false);
  save("01-prompt");

  // Scene 2: revealed, with interval labels on the grade buttons.
  ui::show_card({"咖啡", "kāfēi", "cà phê"}, true);
  ui::set_intervals({"1m", "6m", "10m", "4d"});
  save("02-revealed");

  // Scene 3: a long gloss, to check wrapping doesn't overflow the panel.
  ui::show_card({"吧", "ba", "(trợ từ: đề nghị hoặc rủ rê - ...nhé, ...đi); quán bar"}, true);
  ui::set_intervals({"1m", "8m", "12m", "5d"});
  save("03-long-gloss");

  // Scene 4: a multi-character headword at the large font size.
  ui::show_card({"不客气", "bú kèqi", "không có gì; đừng khách sáo"}, true);
  ui::set_intervals({"1m", "10m", "1d", "3d"});
  save("04-long-headword");

  // Scene 5: nothing left to review.
  ui::show_done(42);
  save("05-done");

  // Regression scenes for state-leak bugs: a style/font set in one state
  // must not bleed into a later, unrelated state.

  // Scene 6: after a long headword was shown revealed (scene 4 shrank
  // g.front to font_cjk_28), a fresh short-headword PROMPT must render at
  // full size again, not inherit the shrunk font.
  ui::show_card({"你好", "nǐ hǎo", "xin chào"}, false);
  save("06-prompt-after-long-headword");

  // Scene 7: same short headword, now revealed, should also be full size.
  ui::show_card({"你好", "nǐ hǎo", "xin chào"}, true);
  ui::set_intervals({"1m", "6m", "10m", "4d"});
  save("07-revealed-after-long-headword");

  // Scene 8: after a long gloss shrank g.back (scene 3), show_done reuses
  // g.back for "All done" -- must not render in the shrunk gloss font.
  ui::show_card({"吧", "ba", "(trợ từ: đề nghị hoặc rủ rê - ...nhé, ...đi); quán bar"}, true);
  ui::set_intervals({"1m", "8m", "12m", "5d"});
  ui::show_done(7);
  save("08-done-after-long-gloss");

  // Scene 9: leaving done and revealing a short card must restore normal
  // fonts on both front and back (done pins front to 48 / back to 28, but
  // confirm a real card still fits/re-fits correctly afterward).
  ui::show_card({"猫", "māo", "con mèo"}, true);
  ui::set_intervals({"1m", "6m", "10m", "4d"});
  save("09-revealed-after-done");

  // --- Home screen and multi-deck picker ------------------------------------
  // Exercises deck_registry.h's scan()/parse_meta()/counts_for() against the
  // real compiled decks in decks/ -- not a fixture invented for the test, per
  // the instruction to avoid running tools/deckc.py here. hsk1-2.srs (zh) and
  // french-a1.srs (fr) are exactly the pairing needed to also demonstrate
  // lang-gated tone colouring (docs/sync-protocol.md) below.
  static fsrs::Parameters params;
  static session::Limits limits;
  std::vector<session::ReviewEntry> no_history;  // fresh device, nothing reviewed yet

  auto files = deck::scan(g_decks_dir.c_str());
  std::printf("scanned %s: %zu deck(s)\n", g_decks_dir.c_str(), files.size());

  // Keeps every deck's bytes and Deck view alive for the rest of main(), the
  // same lifetime rule main.cpp's g_deck_bytes follows on-device.
  struct Loaded {
    std::string slug;
    std::vector<uint8_t> bytes;
    deck::Deck deck;
    deck::Meta meta;
  };
  std::vector<Loaded> loaded;
  for (const auto& f : files) {
    Loaded l;
    l.slug = f.slug;
    if (!deck::read_file(f.path.c_str(), &l.bytes)) {
      std::printf("  %s: read failed\n", f.path.c_str());
      continue;
    }
    if (l.deck.open(l.bytes.data(), l.bytes.size()) != deck::Error::None) {
      std::printf("  %s: open failed\n", f.path.c_str());
      continue;
    }
    l.meta = deck::parse_meta(l.deck.meta(), l.deck.meta_len());
    std::printf("  %s: name=\"%s\" lang=%s cards=%zu\n", l.slug.c_str(),
                l.meta.name.c_str(), l.meta.lang.c_str(), l.deck.count());
    loaded.push_back(std::move(l));
  }

  home::init(g_hor, g_ver);
  home::set_select_callback([](int) {});  // no interaction in this offline render pass
  home::set_refresh_callback([]() {});

  std::vector<home::DeckRow> rows;
  for (const auto& l : loaded) {
    const auto c = deck::counts_for(l.deck, params, limits, no_history, 0, 7 * 3600);
    rows.push_back({l.meta.name.c_str(), c.fresh, c.learning, c.due});
  }
  home::set_decks(rows.data(), static_cast<int>(rows.size()));

  // Scene 10: clock unset, battery unknown -- the honest defaults before any
  // sync has ever set the clock and before the ADC channel exists (see
  // components/power/). "--:--" and "?" are what should be on the glass now,
  // not a guessed time or percentage.
  home::set_status({-1, -1, -1, false});
  save("10-home-unset-clock-unknown-battery");

  // Scene 11: same picker once the clock and battery are known, so the
  // status-bar layout can be checked with real-looking values too.
  home::set_status({14, 32, 76, true});
  save("11-home-with-clock-and-battery");

  // Scene 12: opening the French deck. lang=fr must render the reading
  // (IPA, not pinyin) in one neutral colour -- no tone marks to key off of,
  // and none should be invented.
  if (!loaded.empty()) {
    const auto& fr = loaded.front();  // "french-a1" sorts first
    session::Session fr_sess(fr.deck, params, limits);
    const int idx = fr_sess.next_card(0, 7 * 3600);
    if (idx >= 0) {
      const auto card = fr.deck.at(static_cast<size_t>(idx));
      static char front[64], reading[96], back[256];
      auto copy = [](char* dst, size_t cap, const deck::Str& s) {
        const size_t n = s.len < cap - 1 ? s.len : cap - 1;
        if (s.data && n) std::memcpy(dst, s.data, n);
        dst[n] = '\0';
      };
      copy(front, sizeof(front), card.front);
      copy(reading, sizeof(reading), card.reading);
      copy(back, sizeof(back), card.back);
      ui::set_deck_name(fr.meta.name.c_str());
      ui::set_lang(fr.meta.lang.c_str());
      ui::show_card({front, reading, back}, true);
      lv_screen_load(ui::screen());
      save("12-review-french-neutral-reading");
    }
  }

  // Scene 13: back to a zh card after a non-zh deck was open, to confirm
  // set_lang() actually re-enables tone colouring rather than leaving it
  // stuck off from the previous deck.
  ui::set_deck_name("HSK 1-2");
  ui::set_lang("zh");
  ui::show_card({"你好", "nǐ hǎo", "xin chào"}, true);
  ui::set_intervals({"1m", "6m", "10m", "4d"});
  save("13-review-zh-after-french");

  // Scene 14: the back arrow's destination -- the picker again, refreshed.
  lv_screen_load(home::screen());
  save("14-home-after-review");

  std::printf("done\n");
  return 0;
}
