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

#include "review_ui.h"

namespace {

int g_hor = 240;
int g_ver = 320;
std::vector<uint16_t> g_framebuffer;
std::string g_outdir = "out";

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
  ui::show_card({"咖啡", "kāfēi", "coffee"}, false);
  save("01-prompt");

  // Scene 2: revealed, with interval labels on the grade buttons.
  ui::show_card({"咖啡", "kāfēi", "coffee"}, true);
  ui::set_intervals({"1m", "6m", "10m", "4d"});
  save("02-revealed");

  // Scene 3: a long gloss, to check wrapping doesn't overflow the panel.
  ui::show_card({"就", "jiù", "then; at once; just; only; with regard to"}, true);
  ui::set_intervals({"1m", "8m", "12m", "5d"});
  save("03-long-gloss");

  // Scene 4: a multi-character headword at the large font size.
  ui::show_card({"不客气", "bú kèqi", "you're welcome; don't be polite"}, true);
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
  ui::show_card({"你好", "nǐ hǎo", "hello"}, false);
  save("06-prompt-after-long-headword");

  // Scene 7: same short headword, now revealed, should also be full size.
  ui::show_card({"你好", "nǐ hǎo", "hello"}, true);
  ui::set_intervals({"1m", "6m", "10m", "4d"});
  save("07-revealed-after-long-headword");

  // Scene 8: after a long gloss shrank g.back (scene 3), show_done reuses
  // g.back for "All done" -- must not render in the shrunk gloss font.
  ui::show_card({"就", "jiù", "then; at once; just; only; with regard to"}, true);
  ui::set_intervals({"1m", "8m", "12m", "5d"});
  ui::show_done(7);
  save("08-done-after-long-gloss");

  // Scene 9: leaving done and revealing a short card must restore normal
  // fonts on both front and back (done pins front to 48 / back to 28, but
  // confirm a real card still fits/re-fits correctly afterward).
  ui::show_card({"猫", "māo", "cat"}, true);
  ui::set_intervals({"1m", "6m", "10m", "4d"});
  save("09-revealed-after-done");

  std::printf("done\n");
  return 0;
}
