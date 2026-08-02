#include "rtfont.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char* TAG = "rtfont";

// One sequential read per window during prewarm. The whole window is parsed
// with the font lock held, so this trades a longer worst-case stall in the
// LVGL task (a few ms) against far fewer LittleFS reads.
#define RTFONT_WINDOW 16384

// --- on-disk structures (lv_binfont_loader.c) -------------------------------

typedef struct {
  uint32_t version;
  uint16_t tables_count;
  uint16_t font_size;
  uint16_t ascent;
  int16_t descent;
  uint16_t typo_ascent;
  int16_t typo_descent;
  uint16_t typo_line_gap;
  int16_t min_y;
  int16_t max_y;
  uint16_t default_advance_width;
  uint16_t kerning_scale;
  uint8_t index_to_loc_format;
  uint8_t glyph_id_format;
  uint8_t advance_width_format;
  uint8_t bits_per_pixel;
  uint8_t xy_bits;
  uint8_t wh_bits;
  uint8_t advance_width_bits;
  uint8_t compression_id;
  uint8_t subpixels_mode;
  uint8_t padding;
  int16_t underline_position;
  uint16_t underline_thickness;
} font_header_bin_t;

typedef struct {
  uint32_t data_offset;
  uint32_t range_start;
  uint16_t range_length;
  uint16_t glyph_id_start;
  uint16_t data_entries_count;
  uint8_t format_type;
  uint8_t padding;
} cmap_table_bin_t;

// --- runtime state ----------------------------------------------------------

// `fmt` is first so that font->dsc (which LVGL's fmt_txt callbacks require to
// point at an lv_font_fmt_txt_dsc_t) doubles as the handle to everything else.
typedef struct {
  lv_font_fmt_txt_dsc_t fmt;

  FILE* fp;
  SemaphoreHandle_t lock;

  uint32_t glyf_start;  // file offset of the "glyf" table label
  uint32_t glyf_len;    // its length, and the size of the bitmap arena
  uint32_t loca_count;
  uint32_t* loca;       // loca_count + 1 entries; the last is glyf_len
  uint8_t* loaded;      // one byte per glyph, release/acquire published
  uint8_t* arena;       // == fmt.glyph_bitmap
  lv_font_fmt_txt_glyph_dsc_t* gdsc;  // == fmt.glyph_dsc

  uint32_t max_entry;  // largest loca delta, sizes the on-demand read buffer
  uint8_t* entry_buf;

  uint8_t adv_w_bits;
  uint8_t adv_w_format;
  uint8_t xy_bits;
  uint8_t wh_bits;
  uint16_t default_adv_w;
} rtfont_t;

// --- allocation -------------------------------------------------------------

// PSRAM by preference: a full set of deck fonts is several MB of cached
// glyphs and internal SRAM cannot hold it. CONFIG_SPIRAM_USE_MALLOC routes
// plain malloc() to PSRAM too, but only above a size threshold, so ask for
// the capability explicitly and fall back for the small tables.
static void* rt_alloc(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}

static void* rt_calloc(size_t n) {
  void* p = rt_alloc(n);
  if (p) memset(p, 0, n);
  return p;
}

// --- file helpers -----------------------------------------------------------

static bool rd(FILE* fp, void* dst, size_t n) { return fread(dst, 1, n, fp) == n; }

static bool read_label(FILE* fp, uint32_t start, const char* label, uint32_t* len_out) {
  char buf[4];
  if (fseek(fp, (long)start, SEEK_SET) != 0) return false;
  if (!rd(fp, len_out, 4) || !rd(fp, buf, 4) || memcmp(label, buf, 4) != 0) return false;
  return true;
}

// --- bit reader -------------------------------------------------------------

// MSB-first over a byte buffer, matching lv_binfont_loader.c's read_bits(),
// which walks the file one bit at a time from the high bit of each byte.
typedef struct {
  const uint8_t* p;
  size_t len;
  uint32_t bit;
} bitrd_t;

static uint32_t rd_bits(bitrd_t* b, int n) {
  uint32_t v = 0;
  while (n--) {
    const size_t byte_i = b->bit >> 3;
    const uint32_t bit = byte_i < b->len ? ((b->p[byte_i] >> (7 - (b->bit & 7))) & 1u) : 0u;
    v |= bit << n;
    b->bit++;
  }
  return v;
}

static int32_t rd_bits_signed(bitrd_t* b, int n) {
  uint32_t v = rd_bits(b, n);
  if (n > 0 && (v & (1u << (n - 1)))) v |= ~0u << n;
  return (int32_t)v;
}

// --- glyph parsing ----------------------------------------------------------

// Decodes one glyph entry out of `src` (the raw bytes of loca[gid] ..
// loca[gid + 1]) into the cache. Mirrors load_glyph()'s two passes in
// lv_binfont_loader.c, collapsed into one because we already know where the
// bitmap goes: rather than packing bitmaps end-to-end in a blob whose layout
// needs every glyph's box size up front, each glyph's bitmap lives at its own
// loca offset inside an arena the size of the whole glyf section. That wastes
// only the few header bytes per glyph and needs nothing but loca to compute.
static void parse_glyph(rtfont_t* rt, uint32_t gid, const uint8_t* src, size_t src_len) {
  const uint32_t off = rt->loca[gid];
  const uint32_t next = rt->loca[gid + 1];
  lv_font_fmt_txt_glyph_dsc_t* g = &rt->gdsc[gid];

  bitrd_t b = {src, src_len, 0};

  uint32_t adv_w;
  if (rt->adv_w_bits == 0) adv_w = rt->default_adv_w;
  else adv_w = rd_bits(&b, rt->adv_w_bits);
  if (rt->adv_w_format == 0) adv_w *= 16;

  const int32_t ofs_x = rd_bits_signed(&b, rt->xy_bits);
  const int32_t ofs_y = rd_bits_signed(&b, rt->xy_bits);
  const uint32_t box_w = rd_bits(&b, rt->wh_bits);
  const uint32_t box_h = rd_bits(&b, rt->wh_bits);

  const int nbits = rt->adv_w_bits + 2 * rt->xy_bits + 2 * rt->wh_bits;
  const int bmp_size = (int)(next - off) - nbits / 8;

  if (gid == 0) {
    g->adv_w = 0;
    g->box_w = 0;
    g->box_h = 0;
    g->ofs_x = 0;
    g->ofs_y = 0;
  } else {
    g->adv_w = adv_w;
    g->box_w = box_w;
    g->box_h = box_h;
    g->ofs_x = ofs_x;
    g->ofs_y = ofs_y;
  }
  g->bitmap_index = off;

  if (gid == 0 || box_w * box_h == 0 || bmp_size <= 0) return;

  uint8_t* out = rt->arena + off;
  if (nbits % 8 == 0) {
    memcpy(out, src + nbits / 8, (size_t)bmp_size);
  } else {
    // The bitmap starts mid-byte, so every output byte straddles two input
    // bytes. The trailing fragment is short and read_bits() right-aligns it,
    // so it has to be shifted back onto the MSB.
    for (int k = 0; k < bmp_size - 1; ++k) out[k] = (uint8_t)rd_bits(&b, 8);
    out[bmp_size - 1] = (uint8_t)(rd_bits(&b, 8 - nbits % 8) << (nbits % 8));
  }
}

static bool load_glyph_locked(rtfont_t* rt, uint32_t gid) {
  const uint32_t off = rt->loca[gid];
  const uint32_t len = rt->loca[gid + 1] - off;
  if (len > rt->max_entry) return false;
  if (fseek(rt->fp, (long)(rt->glyf_start + off), SEEK_SET) != 0) return false;
  if (fread(rt->entry_buf, 1, len, rt->fp) != len) return false;
  parse_glyph(rt, gid, rt->entry_buf, len);
  return true;
}

// --- lv_font_t callbacks ----------------------------------------------------

static bool rt_get_glyph_dsc(const lv_font_t* font, lv_font_glyph_dsc_t* dsc_out, uint32_t letter,
                             uint32_t letter_next) {
  // The first call is only for its side effect of resolving the codepoint
  // through the cmaps into a glyph id; the metrics it fills in are whatever
  // the (possibly still zeroed) cache holds. Once the glyph is in, redo it.
  if (!lv_font_get_glyph_dsc_fmt_txt(font, dsc_out, letter, letter_next)) return false;

  rtfont_t* rt = (rtfont_t*)font->dsc;
  const uint32_t gid = dsc_out->gid.index;
  if (gid >= rt->loca_count) return false;
  if (__atomic_load_n(&rt->loaded[gid], __ATOMIC_ACQUIRE)) return true;

  xSemaphoreTake(rt->lock, portMAX_DELAY);
  bool ok = true;
  if (!rt->loaded[gid]) {
    ok = load_glyph_locked(rt, gid);
    if (ok) __atomic_store_n(&rt->loaded[gid], 1, __ATOMIC_RELEASE);
  }
  xSemaphoreGive(rt->lock);
  if (!ok) return false;

  return lv_font_get_glyph_dsc_fmt_txt(font, dsc_out, letter, letter_next);
}

static void rt_release_glyph(const lv_font_t* font, lv_font_glyph_dsc_t* dsc) {
  LV_UNUSED(font);
  LV_UNUSED(dsc);
}

// --- table loading ----------------------------------------------------------

static bool load_cmaps(rtfont_t* rt, uint32_t cmaps_start, uint32_t* cmaps_len) {
  if (!read_label(rt->fp, cmaps_start, "cmap", cmaps_len)) return false;

  uint32_t count;
  if (!rd(rt->fp, &count, 4) || count == 0) return false;

  lv_font_fmt_txt_cmap_t* cmaps = rt_calloc(count * sizeof(lv_font_fmt_txt_cmap_t));
  cmap_table_bin_t* tables = rt_alloc(count * sizeof(cmap_table_bin_t));
  if (!cmaps || !tables) {
    free(cmaps);
    free(tables);
    return false;
  }
  rt->fmt.cmaps = cmaps;
  rt->fmt.cmap_num = count;

  bool ok = rd(rt->fp, tables, count * sizeof(cmap_table_bin_t));
  for (uint32_t i = 0; ok && i < count; ++i) {
    if (fseek(rt->fp, (long)(cmaps_start + tables[i].data_offset), SEEK_SET) != 0) {
      ok = false;
      break;
    }
    lv_font_fmt_txt_cmap_t* c = &cmaps[i];
    c->range_start = tables[i].range_start;
    c->range_length = tables[i].range_length;
    c->glyph_id_start = tables[i].glyph_id_start;
    c->type = tables[i].format_type;

    switch (tables[i].format_type) {
      case LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL: {
        const uint32_t n = tables[i].data_entries_count;
        uint8_t* ids = rt_alloc(n);
        c->glyph_id_ofs_list = ids;
        c->list_length = c->range_length;
        ok = ids && rd(rt->fp, ids, n);
        break;
      }
      case LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY:
        break;
      case LV_FONT_FMT_TXT_CMAP_SPARSE_FULL:
      case LV_FONT_FMT_TXT_CMAP_SPARSE_TINY: {
        const uint32_t n = tables[i].data_entries_count;
        uint16_t* list = rt_alloc(n * sizeof(uint16_t));
        c->unicode_list = list;
        c->list_length = n;
        ok = list && rd(rt->fp, list, n * sizeof(uint16_t));
        if (ok && tables[i].format_type == LV_FONT_FMT_TXT_CMAP_SPARSE_FULL) {
          uint16_t* ofs = rt_alloc(n * sizeof(uint16_t));
          c->glyph_id_ofs_list = ofs;
          ok = ofs && rd(rt->fp, ofs, n * sizeof(uint16_t));
        }
        break;
      }
      default:
        ESP_LOGW(TAG, "unknown cmap format %d", tables[i].format_type);
        ok = false;
    }
  }
  free(tables);
  return ok;
}

static bool load_kern(rtfont_t* rt, uint32_t start, uint8_t glyph_id_format) {
  uint32_t kern_len;
  if (!read_label(rt->fp, start, "kern", &kern_len)) return false;

  uint8_t kern_format;
  uint8_t pad[3];
  if (!rd(rt->fp, &kern_format, 1) || !rd(rt->fp, pad, 3)) return false;

  if (kern_format == 0) { /*sorted pairs*/
    lv_font_fmt_txt_kern_pair_t* kp = rt_calloc(sizeof(*kp));
    if (!kp) return false;
    rt->fmt.kern_dsc = kp;
    rt->fmt.kern_classes = 0;

    uint32_t pairs;
    if (!rd(rt->fp, &pairs, 4)) return false;
    const size_t ids_size = (glyph_id_format == 0 ? 2u : 4u) * pairs;
    uint8_t* ids = rt_alloc(ids_size);
    int8_t* values = rt_alloc(pairs);
    kp->glyph_ids_size = glyph_id_format;
    kp->pair_cnt = pairs;
    kp->glyph_ids = ids;
    kp->values = values;
    if (!ids || !values) return false;
    return rd(rt->fp, ids, ids_size) && rd(rt->fp, values, pairs);
  }

  if (kern_format == 3) { /*array of classes*/
    lv_font_fmt_txt_kern_classes_t* kc = rt_calloc(sizeof(*kc));
    if (!kc) return false;
    rt->fmt.kern_dsc = kc;
    rt->fmt.kern_classes = 1;

    uint16_t mapping_len;
    uint8_t rows, cols;
    if (!rd(rt->fp, &mapping_len, 2) || !rd(rt->fp, &rows, 1) || !rd(rt->fp, &cols, 1)) return false;
    const size_t values_len = (size_t)rows * cols;
    uint8_t* left = rt_alloc(mapping_len);
    uint8_t* right = rt_alloc(mapping_len);
    int8_t* values = rt_alloc(values_len);
    kc->left_class_mapping = left;
    kc->right_class_mapping = right;
    kc->left_class_cnt = rows;
    kc->right_class_cnt = cols;
    kc->class_pair_values = values;
    if (!left || !right || !values) return false;
    return rd(rt->fp, left, mapping_len) && rd(rt->fp, right, mapping_len) &&
           rd(rt->fp, values, values_len);
  }

  ESP_LOGW(TAG, "unknown kern format %d", kern_format);
  return false;
}

static bool load_tables(rtfont_t* rt, lv_font_t* font) {
  uint32_t head_len;
  if (!read_label(rt->fp, 0, "head", &head_len)) return false;

  font_header_bin_t h;
  if (!rd(rt->fp, &h, sizeof(h))) return false;
  if (h.compression_id != 0) {
    ESP_LOGE(TAG, "compressed fonts are not supported (build with --no-compress)");
    return false;
  }

  font->base_line = -h.descent;
  font->line_height = h.ascent - h.descent;
  font->get_glyph_dsc = rt_get_glyph_dsc;
  font->get_glyph_bitmap = lv_font_get_bitmap_fmt_txt;
  font->release_glyph = rt_release_glyph;
  font->subpx = h.subpixels_mode;
  font->underline_position = (int8_t)h.underline_position;
  font->underline_thickness = (int8_t)h.underline_thickness;

  rt->fmt.bpp = h.bits_per_pixel;
  rt->fmt.kern_scale = h.kerning_scale;
  rt->fmt.bitmap_format = h.compression_id;

  rt->adv_w_bits = h.advance_width_bits;
  rt->adv_w_format = h.advance_width_format;
  rt->xy_bits = h.xy_bits;
  rt->wh_bits = h.wh_bits;
  rt->default_adv_w = h.default_advance_width;

  uint32_t cmaps_len;
  if (!load_cmaps(rt, head_len, &cmaps_len)) return false;

  const uint32_t loca_start = head_len + cmaps_len;
  uint32_t loca_len;
  if (!read_label(rt->fp, loca_start, "loca", &loca_len)) return false;
  if (!rd(rt->fp, &rt->loca_count, 4) || rt->loca_count == 0) return false;

  rt->loca = rt_alloc((rt->loca_count + 1) * sizeof(uint32_t));
  if (!rt->loca) return false;
  if (h.index_to_loc_format == 0) {
    for (uint32_t i = 0; i < rt->loca_count; ++i) {
      uint16_t o;
      if (!rd(rt->fp, &o, 2)) return false;
      rt->loca[i] = o;
    }
  } else if (h.index_to_loc_format == 1) {
    if (!rd(rt->fp, rt->loca, rt->loca_count * sizeof(uint32_t))) return false;
  } else {
    ESP_LOGE(TAG, "unknown index_to_loc_format %d", h.index_to_loc_format);
    return false;
  }

  rt->glyf_start = loca_start + loca_len;
  if (!read_label(rt->fp, rt->glyf_start, "glyf", &rt->glyf_len)) return false;
  rt->loca[rt->loca_count] = rt->glyf_len;

  rt->max_entry = 0;
  for (uint32_t i = 0; i < rt->loca_count; ++i) {
    const uint32_t n = rt->loca[i + 1] - rt->loca[i];
    if (n > rt->max_entry) rt->max_entry = n;
  }

  rt->gdsc = rt_calloc(rt->loca_count * sizeof(lv_font_fmt_txt_glyph_dsc_t));
  rt->loaded = rt_calloc(rt->loca_count);
  rt->arena = rt_alloc(rt->glyf_len);
  rt->entry_buf = rt_alloc(rt->max_entry);
  if (!rt->gdsc || !rt->loaded || !rt->arena || !rt->entry_buf) return false;
  rt->fmt.glyph_dsc = rt->gdsc;
  rt->fmt.glyph_bitmap = rt->arena;

  // tables_count < 4 means the file has no kern table at all.
  if (h.tables_count >= 4) {
    if (!load_kern(rt, rt->glyf_start + rt->glyf_len, h.glyph_id_format)) return false;
  }
  return true;
}

// --- public API -------------------------------------------------------------

lv_font_t* rtfont_create(const char* path) {
  rtfont_t* rt = rt_calloc(sizeof(rtfont_t));
  lv_font_t* font = rt_calloc(sizeof(lv_font_t));
  if (!rt || !font) {
    free(rt);
    free(font);
    return NULL;
  }
  font->dsc = &rt->fmt;
  font->user_data = rt;

  rt->lock = xSemaphoreCreateMutex();
  rt->fp = fopen(path, "rb");
  if (!rt->lock || !rt->fp || !load_tables(rt, font)) {
    rtfont_destroy(font);
    return NULL;
  }
  return font;
}

void rtfont_destroy(lv_font_t* font) {
  if (!font) return;
  rtfont_t* rt = (rtfont_t*)font->dsc;
  if (rt) {
    if (rt->fmt.kern_classes) {
      const lv_font_fmt_txt_kern_classes_t* kc = rt->fmt.kern_dsc;
      if (kc) {
        free((void*)kc->class_pair_values);
        free((void*)kc->left_class_mapping);
        free((void*)kc->right_class_mapping);
        free((void*)kc);
      }
    } else {
      const lv_font_fmt_txt_kern_pair_t* kp = rt->fmt.kern_dsc;
      if (kp) {
        free((void*)kp->glyph_ids);
        free((void*)kp->values);
        free((void*)kp);
      }
    }
    if (rt->fmt.cmaps) {
      for (uint32_t i = 0; i < rt->fmt.cmap_num; ++i) {
        free((void*)rt->fmt.cmaps[i].glyph_id_ofs_list);
        free((void*)rt->fmt.cmaps[i].unicode_list);
      }
      free((void*)rt->fmt.cmaps);
    }
    free(rt->arena);
    free(rt->gdsc);
    free(rt->loaded);
    free(rt->loca);
    free(rt->entry_buf);
    if (rt->fp) fclose(rt->fp);
    if (rt->lock) vSemaphoreDelete(rt->lock);
    free(rt);
  }
  free(font);
}

bool rtfont_prewarm(lv_font_t* font) {
  if (!font) return false;
  rtfont_t* rt = (rtfont_t*)font->dsc;

  size_t win_size = RTFONT_WINDOW;
  if (rt->max_entry > win_size) win_size = rt->max_entry;
  uint8_t* win = rt_alloc(win_size);
  if (!win) return false;

  bool ok = true;
  uint32_t gid = 0;
  int windows = 0;
  while (gid < rt->loca_count) {
    xSemaphoreTake(rt->lock, portMAX_DELAY);

    const uint32_t win_start = rt->loca[gid];
    size_t n = rt->glyf_len - win_start;
    if (n > win_size) n = win_size;
    if (fseek(rt->fp, (long)(rt->glyf_start + win_start), SEEK_SET) != 0 ||
        fread(win, 1, n, rt->fp) != n) {
      xSemaphoreGive(rt->lock);
      ok = false;
      break;
    }

    // Drain every glyph that fits entirely in the window before releasing the
    // lock, so one file read serves many glyphs. Always advances by at least
    // one glyph: win_size is clamped to be no smaller than the largest entry.
    for (; gid < rt->loca_count && rt->loca[gid + 1] - win_start <= n; ++gid) {
      if (rt->loaded[gid]) continue;
      const uint32_t off = rt->loca[gid] - win_start;
      parse_glyph(rt, gid, win + off, rt->loca[gid + 1] - rt->loca[gid]);
      __atomic_store_n(&rt->loaded[gid], 1, __ATOMIC_RELEASE);
    }

    xSemaphoreGive(rt->lock);
    // Neither the LittleFS read nor the bit unpacking ever blocks, so without
    // an explicit sleep this loop can starve the idle task into a watchdog.
    if ((++windows & 7) == 0) vTaskDelay(1);
    else taskYIELD();
  }

  free(win);
  return ok;
}
