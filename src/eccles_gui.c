/*
 * MIT License
 *
 * Copyright (c) 2025-2026 Nwobodo Ecclesiastes Chidera
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

//eccles_gui.c the implementation: band renderer, 2d primitives, text, widgets, input, animation and the 3d rasteriser
//in this order: 1 helpers + blending, 2 context + dirty rects + band loop, 3 2d primitives, 4 triangle rasteriser,
//5 text + images, 6 widgets, 7 input, 8 animation, 9 3d, 10 generated data (font, sine, meshes)
//the generated data lives at the bottom and is written by tools/gen_data.py, do not edit those blocks by hand

#include "eccles_gui.h"
#include <string.h>

//debug logging, if you define ECCLES_GUI_DEBUG it prints to the console (only misuse and failures, never the happy path)
#if defined(ECCLES_GUI_DEBUG)
  #ifndef ECCLES_GUI_LOG_LINE
    #include <stdio.h>
    #define ECCLES_GUI_LOG_LINE(msg) printf("[eccles_gui] %s\n", (msg))
  #endif
#else
  #ifndef ECCLES_GUI_LOG_LINE
    #define ECCLES_GUI_LOG_LINE(msg) ((void)0)
  #endif
#endif

//ECCLES_GUI_EXEC_IRAM puts the hot pixel loops (spans, blends, the triangle rasteriser, glyph blits) in internal ram
//the attribute depends on the chip, you can always set ECCLES_GUI_IRAM_ATTR yourself (same thing for chips i dont know about)
#if ECCLES_GUI_EXEC_IRAM
  #if ECCLES_GUI_IS_ESP
    #include "esp_attr.h"
  #endif
  #ifndef ECCLES_GUI_IRAM_ATTR
    #if ECCLES_GUI_IS_ESP
      #define ECCLES_GUI_IRAM_ATTR IRAM_ATTR
    #elif ECCLES_GUI_PLATFORM == ECCLES_GUI_PLAT_PICO
      #define ECCLES_GUI_IRAM_ATTR __attribute__((section(".time_critical.eccles_gui")))
    #elif ECCLES_GUI_PLATFORM == ECCLES_GUI_PLAT_STM32_HAL
      #define ECCLES_GUI_IRAM_ATTR __attribute__((section(".RamFunc")))
    #elif ECCLES_GUI_PLATFORM == ECCLES_GUI_PLAT_ZEPHYR
      #include <zephyr/kernel.h>
      #define ECCLES_GUI_IRAM_ATTR __ramfunc
    #elif ECCLES_GUI_PLATFORM == ECCLES_GUI_PLAT_HOST
      #define ECCLES_GUI_IRAM_ATTR
    #else
      #error "ECCLES_GUI_EXEC_IRAM: dont know the ram function attribute of this chip, define ECCLES_GUI_IRAM_ATTR yourself"
    #endif
  #endif
  #define G_HOT ECCLES_GUI_IRAM_ATTR
#else
  #define G_HOT
#endif

#if defined(__GNUC__) || defined(__clang__)
  #define G_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
  #define G_UNLIKELY(x) (x)
#endif

//shr is written as a C >> on signed values, that is implementation defined in iso c but arithmetic on every compiler i target
//(gcc, clang, msvc, iar, keil, ti, xc). when a compiler is not like that the build fails right here
typedef char g_asr32_check[((-1) >> 1) == -1 ? 1 : -1];

#define G_MIN(a, b) ((a) < (b) ? (a) : (b))
#define G_MAX(a, b) ((a) > (b) ? (a) : (b))
#define G_ABS(a) ((a) < 0 ? -(a) : (a))
#define G_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))

//1. helpers + blending

//integer square root (floor), no division, 16 iterations
static uint32_t g_isqrt(uint32_t v) {
    uint32_t res = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= res + bit) { v -= res + bit; res = (res >> 1) + bit; }
        else res >>= 1;
        bit >>= 2;
    }
    return res;
}

//rgb565 in "spread" form: 00000gggggg00000rrrrr000000bbbbb, lets one multiply blend all 3 channels
#define G_SPREAD_MASK 0x07E0F81Fu
static inline uint32_t g_spread(uint16_t c) { uint32_t s = c; return (s | (s << 16)) & G_SPREAD_MASK; }
static inline uint16_t g_unspread(uint32_t s) { s &= G_SPREAD_MASK; return (uint16_t)(s | (s >> 16)); }
//d + (s - d) * a5 / 32 for all channels at once. s is spread, a5 is 0..32
static inline uint16_t g_blend_s(uint16_t d, uint32_t s, uint32_t a5) {
    uint32_t dd = g_spread(d);
    return g_unspread(dd + (((s - dd) * a5) >> 5));
}
static inline uint32_t g_a5(uint32_t a) { return (a + 4u) >> 3; }  //0..255 -> 0..32
static inline uint16_t g_blend(uint16_t d, uint16_t s, uint32_t a) { return g_blend_s(d, g_spread(s), g_a5(a)); }
//color * k / 32, k is 0..32
static inline uint16_t g_scale5(uint16_t c, uint32_t k5) { return g_unspread((g_spread(c) * k5) >> 5); }
static inline uint32_t g_opa_mul(uint32_t a, uint32_t b) { return (a * b + 127u) / 255u; }

eccles_gui_color_t eccles_gui_color_mix(eccles_gui_color_t a, eccles_gui_color_t b, uint8_t t) {
    return g_blend(a, b, t);
}

//error helpers. every public function that can fail sets these
static void g_err(eccles_gui_t *gui, eccles_gui_error_t e, const char *s) {
    if (!gui) return;
    gui->last_error = e;
    gui->error_str = s ? s : "";
    if (e != ECCLES_GUI_OK) ECCLES_GUI_LOG_LINE(eccles_gui_error_name(e));
}

const char *eccles_gui_error_name(eccles_gui_error_t e) {
    switch (e) {
    case ECCLES_GUI_OK: return "OK";
    case ECCLES_GUI_ERR_BAD_ARG: return "ERR_BAD_ARG";
    case ECCLES_GUI_ERR_NO_MEMORY: return "ERR_NO_MEMORY";
    case ECCLES_GUI_ERR_BAD_OBJECT: return "ERR_BAD_OBJECT";
    case ECCLES_GUI_ERR_BAD_TYPE: return "ERR_BAD_TYPE";
    case ECCLES_GUI_ERR_BUFFER: return "ERR_BUFFER";
    case ECCLES_GUI_ERR_NO_FLUSH: return "ERR_NO_FLUSH";
    case ECCLES_GUI_ERR_DISABLED: return "ERR_DISABLED";
    case ECCLES_GUI_ERR_LIMIT: return "ERR_LIMIT";
    }
    return "ERR_UNKNOWN";
}

eccles_gui_error_t eccles_gui_last_error(const eccles_gui_t *gui) { return gui ? gui->last_error : ECCLES_GUI_ERR_BAD_ARG; }
const char *eccles_gui_last_error_str(const eccles_gui_t *gui) { return (gui && gui->error_str) ? gui->error_str : ""; }

//areas (half open)
static inline bool g_area_empty(const eccles_gui_area_t *a) { return a->x2 <= a->x1 || a->y2 <= a->y1; }
static inline eccles_gui_area_t g_area_isect(const eccles_gui_area_t *a, const eccles_gui_area_t *b) {
    eccles_gui_area_t r;
    r.x1 = G_MAX(a->x1, b->x1); r.y1 = G_MAX(a->y1, b->y1);
    r.x2 = G_MIN(a->x2, b->x2); r.y2 = G_MIN(a->y2, b->y2);
    return r;
}
static inline eccles_gui_area_t g_area_union(const eccles_gui_area_t *a, const eccles_gui_area_t *b) {
    eccles_gui_area_t r;
    r.x1 = G_MIN(a->x1, b->x1); r.y1 = G_MIN(a->y1, b->y1);
    r.x2 = G_MAX(a->x2, b->x2); r.y2 = G_MAX(a->y2, b->y2);
    return r;
}
static inline int32_t g_area_size(const eccles_gui_area_t *a) { return (int32_t)(a->x2 - a->x1) * (int32_t)(a->y2 - a->y1); }
static inline bool g_area_overlap(const eccles_gui_area_t *a, const eccles_gui_area_t *b) {
    return a->x1 < b->x2 && b->x1 < a->x2 && a->y1 < b->y2 && b->y1 < a->y2;
}
static inline eccles_gui_area_t g_area_make(int x, int y, int w, int h) {
    eccles_gui_area_t a;
    int x2 = x + w, y2 = y + h;
    a.x1 = (int16_t)G_CLAMP(x, -32768, 32767); a.y1 = (int16_t)G_CLAMP(y, -32768, 32767);
    a.x2 = (int16_t)G_CLAMP(x2, -32768, 32767); a.y2 = (int16_t)G_CLAMP(y2, -32768, 32767);
    return a;
}

//2. context, dirty rectangles, the band loop

//the builtin font is in slots 0..3 (x1..x4), see the end of this file
static void g_flush_wait(eccles_gui_t *gui) {
    if (gui->pending) {
        if (gui->d.flush_wait) gui->d.flush_wait(gui->d.user);
        gui->pending = 0;
    }
}

eccles_gui_status_t eccles_gui_init(eccles_gui_t *gui, const eccles_gui_desc_t *desc) {
    if (!gui) return ECCLES_GUI_FAILED;
    if (!desc) { memset(gui, 0, sizeof *gui); g_err(gui, ECCLES_GUI_ERR_BAD_ARG, "desc"); return ECCLES_GUI_FAILED; }
    eccles_gui_desc_t d = *desc;  //desc may live inside gui
    memset(gui, 0, sizeof *gui);
    if (d.width < 1 || d.height < 1) { g_err(gui, ECCLES_GUI_ERR_BAD_ARG, "size"); return ECCLES_GUI_FAILED; }
    if (!d.buf || d.buf_pixels < (size_t)d.width) { g_err(gui, ECCLES_GUI_ERR_BUFFER, "buf"); return ECCLES_GUI_FAILED; }
    if (!d.flush) { g_err(gui, ECCLES_GUI_ERR_NO_FLUSH, "flush"); return ECCLES_GUI_FAILED; }
    if ((d.flags & ECCLES_GUI_DESC_ASYNC_FLUSH) && !d.flush_wait) { g_err(gui, ECCLES_GUI_ERR_BAD_ARG, "flush_wait"); return ECCLES_GUI_FAILED; }
    gui->d = d;
#if ECCLES_GUI_SWAP_BYTES
    gui->d.flags |= ECCLES_GUI_DESC_SWAP_BYTES;
#endif
    gui->stats.ram_bytes = (uint32_t)sizeof *gui;
    gui->nfonts = 4;
#if ECCLES_GUI_HAS_BUILTIN_FONT
    for (int i = 0; i < 4; i++) { gui->fonts[i].f = &eccles_gui_font_5x8; gui->fonts[i].scale = (uint8_t)(i + 1); }
#endif
#if ECCLES_GUI_HAS_WIDGETS
    //root widget: the whole panel
    gui->nwidgets = 1;
    gui->w[0].type = ECCLES_GUI_W_PANEL;
    gui->w[0].parent = gui->w[0].child = gui->w[0].next = ECCLES_GUI_NO_OBJ;
    gui->w[0].w = d.width; gui->w[0].h = d.height;
    gui->w[0].opa = 255;
    gui->w[0].flags = ECCLES_GUI_F_CLICKABLE;
    gui->w[0].style = ECCLES_GUI_STYLE_SCREEN;
    gui->t_obj = ECCLES_GUI_NO_OBJ;
    gui->fling_obj = ECCLES_GUI_NO_OBJ;
    gui->nstyles = ECCLES_GUI_STYLE_USER;
    eccles_gui_theme_apply(gui, &eccles_gui_palette_dark);
    gui->stats.widgets_used = gui->stats.widgets_peak = 1;
#endif
    gui->ready = 1;
    eccles_gui_invalidate_all(gui);
    return ECCLES_GUI_SUCCESSFUL;
}

void eccles_gui_deinit(eccles_gui_t *gui) {
    if (!gui) return;
    memset(gui, 0, sizeof *gui);
}

eccles_gui_stats_t eccles_gui_get_stats(const eccles_gui_t *gui) {
    eccles_gui_stats_t z;
    memset(&z, 0, sizeof z);
    if (!gui) return z;
    z = gui->stats;
#if ECCLES_GUI_HAS_ANIM && ECCLES_GUI_MAX_ANIMS > 0
    z.anims_active = (uint8_t)eccles_gui_anim_count(gui);
#endif
    return z;
}

int eccles_gui_font_register(eccles_gui_t *gui, const eccles_gui_font_t *font, uint8_t scale) {
    if (!gui || !font || !font->glyphs || !font->bitmap || !font->ranges || scale < 1 || scale > 8
        || (font->bpp != 1 && font->bpp != 4 && font->bpp != 8)) { g_err(gui, ECCLES_GUI_ERR_BAD_ARG, "font"); return -1; }
    if (gui->nfonts >= ECCLES_GUI_MAX_FONTS) { g_err(gui, ECCLES_GUI_ERR_NO_MEMORY, "fonts"); return -1; }
    gui->fonts[gui->nfonts].f = font;
    gui->fonts[gui->nfonts].scale = scale;
    return gui->nfonts++;
}

//dirty rectangles: a short list. a new one is merged with every one it touches (or that it is cheap to merge with)
static void g_dirty_remove(eccles_gui_t *gui, int i) {
    for (int k = i; k + 1 < gui->ndirty; k++) gui->dirty[k] = gui->dirty[k + 1];
    gui->ndirty--;
}

static void g_dirty_add(eccles_gui_t *gui, eccles_gui_area_t a) {
    eccles_gui_area_t scr = g_area_make(0, 0, gui->d.width, gui->d.height);
    a = g_area_isect(&a, &scr);
    if (g_area_empty(&a)) return;
    for (;;) {
        bool merged = false;
        for (int i = 0; i < gui->ndirty; i++) {
            eccles_gui_area_t u = g_area_union(&a, &gui->dirty[i]);
            int32_t sa = g_area_size(&a), sb = g_area_size(&gui->dirty[i]);
            if (g_area_overlap(&a, &gui->dirty[i]) || g_area_size(&u) <= sa + sb + ((sa + sb) >> 2)) {
                a = u;
                g_dirty_remove(gui, i);
                merged = true;
                break;
            }
        }
        if (merged) continue;
        if (gui->ndirty < ECCLES_GUI_MAX_DIRTY) break;
        //full: merge with the one that grows the least, then try again
        int best = 0;
        int32_t best_cost = 0x7FFFFFFF;
        for (int i = 0; i < gui->ndirty; i++) {
            eccles_gui_area_t u = g_area_union(&a, &gui->dirty[i]);
            int32_t cost = g_area_size(&u) - g_area_size(&gui->dirty[i]);
            if (cost < best_cost) { best_cost = cost; best = i; }
        }
        a = g_area_union(&a, &gui->dirty[best]);
        g_dirty_remove(gui, best);
    }
    gui->dirty[gui->ndirty++] = a;
    if (gui->ndirty > gui->stats.dirty_peak) gui->stats.dirty_peak = gui->ndirty;
}

void eccles_gui_invalidate(eccles_gui_t *gui, int x, int y, int w, int h) {
    if (!gui || !gui->ready) return;
    g_dirty_add(gui, g_area_make(x, y, w, h));
}
void eccles_gui_invalidate_all(eccles_gui_t *gui) {
    if (!gui || !gui->ready) return;
    gui->ndirty = 0;
    g_dirty_add(gui, g_area_make(0, 0, gui->d.width, gui->d.height));
}
bool eccles_gui_is_dirty(const eccles_gui_t *gui) { return gui && gui->ndirty > 0; }

//sends one finished band to the panel
static void g_flush_band(eccles_gui_t *gui, uint16_t *buf, int x, int y, int w, int h) {
    size_t n = (size_t)w * (size_t)h;
    if (gui->d.flags & ECCLES_GUI_DESC_SWAP_BYTES) {
        for (size_t i = 0; i < n; i++) buf[i] = (uint16_t)((buf[i] << 8) | (buf[i] >> 8));
    }
    g_flush_wait(gui);  //one transfer at a time
    gui->d.flush(gui->d.user, x, y, w, h, buf);
    gui->stats.flushes++;
    gui->stats.pixels_flushed += (uint32_t)n;
    if (gui->d.flags & ECCLES_GUI_DESC_ASYNC_FLUSH) gui->pending = (uint8_t)(1u | ((uint32_t)gui->cur << 1));
    if (gui->d.buf2) gui->cur ^= 1u;
}

//draws one area band by band
static uint32_t g_render_bands(eccles_gui_t *gui, eccles_gui_area_t area, eccles_gui_draw_fn fn, void *user) {
    eccles_gui_area_t scr = g_area_make(0, 0, gui->d.width, gui->d.height);
    area = g_area_isect(&area, &scr);
    if (g_area_empty(&area) || !fn) return 0;
    int w = area.x2 - area.x1, h = area.y2 - area.y1;
    size_t per = gui->d.buf_pixels / (size_t)w;
    int lines = per > (size_t)h ? h : (int)per;
    if (lines < 1) lines = 1;
    gui->frame++;
    uint32_t before = gui->stats.pixels_flushed;
    for (int y = area.y1; y < area.y2; y += lines) {
        int bh = G_MIN(lines, area.y2 - y);
        uint16_t *buf = (gui->cur && gui->d.buf2) ? gui->d.buf2 : gui->d.buf;
        if ((gui->pending & 1u) && (gui->pending >> 1) == gui->cur) g_flush_wait(gui);  //the buffer is still being sent
        eccles_gui_dc_t dc;
        dc.gui = gui;
        dc.buf = buf;
        dc.zbuf = gui->d.zbuf;
        dc.stride = (int16_t)w;
        dc.x0 = area.x1; dc.y0 = (int16_t)y;
        dc.clip.x1 = area.x1; dc.clip.x2 = area.x2; dc.clip.y1 = (int16_t)y; dc.clip.y2 = (int16_t)(y + bh);
        fn(&dc, &area, user);
        gui->stats.pixels_rendered += (uint32_t)w * (uint32_t)bh;
        g_flush_band(gui, buf, area.x1, y, w, bh);
    }
    g_flush_wait(gui);
    return gui->stats.pixels_flushed - before;
}

uint32_t eccles_gui_render_area(eccles_gui_t *gui, int x, int y, int w, int h, eccles_gui_draw_fn fn, void *user) {
    if (!gui || !gui->ready) return 0;
    uint32_t n = g_render_bands(gui, g_area_make(x, y, w, h), fn, user);
    if (n) gui->stats.frames++;
    return n;
}

#if ECCLES_GUI_HAS_WIDGETS
static void g_draw_tree(eccles_gui_dc_t *dc, const eccles_gui_area_t *area, void *user);
#endif

uint32_t eccles_gui_render(eccles_gui_t *gui) {
    if (!gui || !gui->ready || !gui->ndirty) return 0;
#if ECCLES_GUI_HAS_WIDGETS
    eccles_gui_area_t loc[ECCLES_GUI_MAX_DIRTY];
    int n = gui->ndirty;
    for (int i = 0; i < n; i++) loc[i] = gui->dirty[i];
    gui->ndirty = 0;  //anything invalidated while we draw is for the next pass
    uint32_t total = 0;
    for (int i = 0; i < n; i++) total += g_render_bands(gui, loc[i], g_draw_tree, NULL);
    if (total) gui->stats.frames++;
    return total;
#else
    gui->ndirty = 0;
    return 0;  //no widgets: use eccles_gui_render_area()
#endif
}

//3. 2d primitives

//one horizontal run of one color, clipped. opa 255 is a plain fill, anything else blends
G_HOT static void g_span(eccles_gui_dc_t *dc, int y, int xa, int xb, uint16_t c, uint32_t opa) {
    if (opa == 0 || y < dc->clip.y1 || y >= dc->clip.y2) return;
    if (xa < dc->clip.x1) xa = dc->clip.x1;
    if (xb > dc->clip.x2) xb = dc->clip.x2;
    if (xa >= xb) return;
    uint16_t *p = dc->buf + (size_t)(y - dc->y0) * (size_t)dc->stride + (size_t)(xa - dc->x0);
    int n = xb - xa;
    if (opa >= 255) {
        while (n--) *p++ = c;
    } else {
        uint32_t s = g_spread(c), a5 = g_a5(opa);
        if (a5 == 0) return;
        while (n--) { *p = g_blend_s(*p, s, a5); p++; }
    }
}

G_HOT static inline void g_px(eccles_gui_dc_t *dc, int x, int y, uint16_t c, uint32_t opa) {
    if (x < dc->clip.x1 || x >= dc->clip.x2 || y < dc->clip.y1 || y >= dc->clip.y2 || opa == 0) return;
    uint16_t *p = dc->buf + (size_t)(y - dc->y0) * (size_t)dc->stride + (size_t)(x - dc->x0);
    *p = opa >= 255 ? c : g_blend(*p, c, opa);
}

void eccles_gui_draw_pixel(eccles_gui_dc_t *dc, int x, int y, eccles_gui_color_t c, uint8_t opa) { g_px(dc, x, y, c, opa); }

void eccles_gui_draw_fill(eccles_gui_dc_t *dc, int x, int y, int w, int h, eccles_gui_color_t c, uint8_t opa) {
    if (!dc || w <= 0 || h <= 0) return;
    int y0 = G_MAX(y, dc->clip.y1), y1 = G_MIN(y + h, dc->clip.y2);
    for (int j = y0; j < y1; j++) g_span(dc, j, x, x + w, c, opa);
}

//coverage (0..255) of a pixel at squared distance D from a circle center, everything in half pixels (so r2 = 2r)
//uses the first order signed distance (r2 - D) / (2 r2) so there is no square root anywhere. k = 261120 / r2
static inline int g_covd(int32_t D, int32_t r2, int32_t k) {
    int32_t t = r2 * r2 + 2 * r2 - D;
    int cov;
    if (t <= 0) cov = 0;
    else if (t >= 4 * r2) cov = 255;
    else cov = (int)((t * k) >> 12);
#if !ECCLES_GUI_HAS_AA
    cov = cov >= 128 ? 255 : 0;
#endif
    return cov;
}

//a rounded rectangle in half pixel units
typedef struct { int32_t L, T, R, B, r2, k; } g_rr_t;
static void g_rr_init(g_rr_t *s, int x, int y, int w, int h, int r) {
    s->L = 2 * x; s->T = 2 * y; s->R = 2 * (x + w); s->B = 2 * (y + h);
    s->r2 = 2 * r;
    s->k = r > 0 ? 261120 / (2 * r) : 0;
}
static inline int g_rr_cov(const g_rr_t *s, int px, int py) {
    int32_t X = 2 * px + 1, Y = 2 * py + 1;
    int32_t dl = X - s->L, dr = s->R - X, dt = Y - s->T, db = s->B - Y;
    if (s->r2 > 0 && (dl < s->r2 || dr < s->r2) && (dt < s->r2 || db < s->r2)) {
        int32_t cx = dl < s->r2 ? s->L + s->r2 : s->R - s->r2;
        int32_t cy = dt < s->r2 ? s->T + s->r2 : s->B - s->r2;
        int32_t ddx = X - cx, ddy = Y - cy;
        return g_covd(ddx * ddx + ddy * ddy, s->r2, s->k);
    }
    int32_t m = G_MIN(G_MIN(dl, dr), G_MIN(dt, db));
    return m >= 1 ? 255 : (m <= -1 ? 0 : 127);
}

//what a fill paints: one color or a gradient across the (gx,gy,gw,gh) box
typedef struct { uint16_t c1, c2; uint8_t dir; int16_t gx, gy, gw, gh; } g_fill_t;
static void g_fill_solid(g_fill_t *f, uint16_t c) { f->c1 = f->c2 = c; f->dir = ECCLES_GUI_GRAD_NONE; f->gx = f->gy = f->gw = f->gh = 0; }
static uint16_t g_fcol(const g_fill_t *f, int x, int y) {
    if (f->dir == ECCLES_GUI_GRAD_V) {
        int d = f->gh - 1;
        int t = d > 0 ? ((y - f->gy) * 255) / d : 0;
        return g_blend(f->c1, f->c2, (uint32_t)G_CLAMP(t, 0, 255));
    }
    if (f->dir == ECCLES_GUI_GRAD_H) {
        int d = f->gw - 1;
        int t = d > 0 ? ((x - f->gx) * 255) / d : 0;
        return g_blend(f->c1, f->c2, (uint32_t)G_CLAMP(t, 0, 255));
    }
    return f->c1;
}
G_HOT static void g_fspan(eccles_gui_dc_t *dc, const g_fill_t *f, int y, int xa, int xb, uint32_t opa) {
    if (f->dir != ECCLES_GUI_GRAD_H) { g_span(dc, y, xa, xb, g_fcol(f, xa, y), opa); return; }
    if (opa == 0 || y < dc->clip.y1 || y >= dc->clip.y2) return;
    if (xa < dc->clip.x1) xa = dc->clip.x1;
    if (xb > dc->clip.x2) xb = dc->clip.x2;
    if (xa >= xb) return;
    int32_t d = f->gw - 1;
    int32_t step = d > 0 ? (255 << 16) / d : 0;
    uint16_t *p = dc->buf + (size_t)(y - dc->y0) * (size_t)dc->stride + (size_t)(xa - dc->x0);
    for (int x = xa; x < xb; x++, p++) {
        int32_t t = ((x - f->gx) * step) >> 16;
        uint16_t c = g_blend(f->c1, f->c2, (uint32_t)G_CLAMP(t, 0, 255));
        *p = opa >= 255 ? c : g_blend(*p, c, opa);
    }
}

//one row of a rounded rect that is in a corner zone (or any row: it works for straight ones too)
static void g_rr_row(eccles_gui_dc_t *dc, const g_fill_t *f, const g_rr_t *s, int x, int w, int j, int r, uint32_t opa) {
    int k = 0;
    for (; k < r; k++) {
        int cov = g_rr_cov(s, x + k, j);
        if (cov >= 255) break;
        if (cov) {
            uint32_t o = g_opa_mul(opa, (uint32_t)cov);
            g_px(dc, x + k, j, g_fcol(f, x + k, j), o);
            g_px(dc, x + w - 1 - k, j, g_fcol(f, x + w - 1 - k, j), o);
        }
    }
    g_fspan(dc, f, j, x + k, x + w - k, opa);
}

static int g_rr_radius(int w, int h, int r) {
    if (r < 0) r = 0;
    r = G_MIN(r, G_MIN(w, h) / 2);
    return G_MIN(r, 1000);
}

static void g_fill_rr(eccles_gui_dc_t *dc, const g_fill_t *f, int x, int y, int w, int h, int r, uint32_t opa) {
    if (w <= 0 || h <= 0 || opa == 0) return;
    if (x >= dc->clip.x2 || x + w <= dc->clip.x1 || y >= dc->clip.y2 || y + h <= dc->clip.y1) return;
    r = g_rr_radius(w, h, r);
    int j0 = G_MAX(y, dc->clip.y1), j1 = G_MIN(y + h, dc->clip.y2);
    g_rr_t s;
    g_rr_init(&s, x, y, w, h, r);
    for (int j = j0; j < j1; j++) {
        int rel = j - y;
        if (r == 0 || (rel >= r && rel < h - r)) g_fspan(dc, f, j, x, x + w, opa);
        else g_rr_row(dc, f, &s, x, w, j, r, opa);
    }
}

void eccles_gui_draw_rrect(eccles_gui_dc_t *dc, int x, int y, int w, int h, int r, eccles_gui_color_t c, uint8_t opa) {
    if (!dc) return;
    g_fill_t f;
    g_fill_solid(&f, c);
    g_fill_rr(dc, &f, x, y, w, h, r, opa);
}

void eccles_gui_draw_gradient(eccles_gui_dc_t *dc, int x, int y, int w, int h, int r, eccles_gui_color_t c1, eccles_gui_color_t c2, uint8_t dir, uint8_t opa) {
    if (!dc) return;
    g_fill_t f;
    f.c1 = c1; f.c2 = c2; f.dir = dir > ECCLES_GUI_GRAD_H ? ECCLES_GUI_GRAD_NONE : dir;
    f.gx = (int16_t)x; f.gy = (int16_t)y; f.gw = (int16_t)w; f.gh = (int16_t)h;
    g_fill_rr(dc, &f, x, y, w, h, r, opa);
}

void eccles_gui_draw_disc(eccles_gui_dc_t *dc, int cx, int cy, int r, eccles_gui_color_t c, uint8_t opa) {
    if (r <= 0) return;
    eccles_gui_draw_rrect(dc, cx - r, cy - r, 2 * r, 2 * r, r, c, opa);
}

//the ring between a rounded rect and the same rect shrunk by bw
void eccles_gui_draw_border(eccles_gui_dc_t *dc, int x, int y, int w, int h, int r, int bw, eccles_gui_color_t c, uint8_t opa) {
    if (!dc || bw <= 0 || w <= 0 || h <= 0 || opa == 0) return;
    if (x >= dc->clip.x2 || x + w <= dc->clip.x1 || y >= dc->clip.y2 || y + h <= dc->clip.y1) return;
    g_fill_t f;
    g_fill_solid(&f, c);
    r = g_rr_radius(w, h, r);
    if (w <= 2 * bw || h <= 2 * bw) { g_fill_rr(dc, &f, x, y, w, h, r, opa); return; }
    int ri = G_MAX(r - bw, 0);
    g_rr_t so, si;
    g_rr_init(&so, x, y, w, h, r);
    g_rr_init(&si, x + bw, y + bw, w - 2 * bw, h - 2 * bw, ri);
    int j0 = G_MAX(y, dc->clip.y1), j1 = G_MIN(y + h, dc->clip.y2);
    for (int j = j0; j < j1; j++) {
        int rel = j - y;
        if (rel < bw || rel >= h - bw) {
            if (r == 0 || (rel >= r && rel < h - r)) g_span(dc, j, x, x + w, c, opa);
            else g_rr_row(dc, &f, &so, x, w, j, r, opa);
            continue;
        }
        int half = w / 2;
        for (int k = 0; k < half; k++) {
            int ci = g_rr_cov(&si, x + k, j);
            int co = g_rr_cov(&so, x + k, j);
            int cv = (co * (255 - ci)) / 255;
            if (cv) {
                uint32_t o = g_opa_mul(opa, (uint32_t)cv);
                g_px(dc, x + k, j, c, o);
                g_px(dc, x + w - 1 - k, j, c, o);
            }
            if (ci >= 255) break;
        }
    }
}

void eccles_gui_draw_shadow(eccles_gui_dc_t *dc, int x, int y, int w, int h, int r, int size, uint8_t opa) {
#if ECCLES_GUI_HAS_SHADOW
    if (!dc || size <= 0 || opa == 0) return;
    int n = G_MIN(size, 6);
    int step = G_MAX(1, size / n);
    uint32_t base = g_opa_mul(opa, 110u);
    uint32_t la = base / (uint32_t)n;
    if (la == 0) la = 1;
    int oy = size / 3;
    g_fill_t f;
    g_fill_solid(&f, 0);
    for (int k = n; k >= 1; k--) {
        int e = k * step;
        g_fill_rr(dc, &f, x - e, y - e + oy, w + 2 * e, h + 2 * e, r + e, la);
    }
#else
    (void)dc; (void)x; (void)y; (void)w; (void)h; (void)r; (void)size; (void)opa;
#endif
}

//anti aliased thick line. x major or y major, the width is measured across the line, butt caps on the major axis
void eccles_gui_draw_line(eccles_gui_dc_t *dc, int x0, int y0, int x1, int y1, int width, eccles_gui_color_t c, uint8_t opa) {
    if (!dc || opa == 0) return;
    if (width < 1) width = 1;
    int dx = x1 - x0, dy = y1 - y0;
    if (dx == 0 && dy == 0) {
        eccles_gui_draw_fill(dc, x0 - width / 2, y0 - width / 2, width, width, c, opa);
        return;
    }
    int adx = G_ABS(dx), ady = G_ABS(dy);
    bool steep = ady > adx;
    int major = steep ? ady : adx;
    int len = (int)g_isqrt((uint32_t)((int64_t)dx * dx + (int64_t)dy * dy));
    int32_t ratio = (int32_t)(((int64_t)len << 8) / major);  //Q8, >= 256
    int32_t half = (width * ratio) / 2;  //half the thickness along the minor axis, Q8
    int sm = (steep ? dy : dx) > 0 ? 1 : -1;
    int a0 = steep ? y0 : x0, b0 = steep ? x0 : y0;
    int32_t minor_d = steep ? dx : dy;
    int32_t slope = (int32_t)(((int64_t)minor_d * 4096) / major);  //Q12, minor per major step
    int32_t acc = 0;
    int cmin = steep ? dc->clip.x1 : dc->clip.y1, cmax = steep ? dc->clip.x2 : dc->clip.y2;
    int lmin = steep ? dc->clip.y1 : dc->clip.x1, lmax = steep ? dc->clip.y2 : dc->clip.x2;
    for (int i = 0; i <= major; i++, acc += slope) {
        int p = a0 + sm * i;
        if (p < lmin || p >= lmax) continue;
        int32_t center = b0 * 256 + 128 + (acc >> 4);
        int32_t top = center - half, bot = center + half;
        int j0 = top >> 8, j1 = (bot - 1) >> 8;
        if (j0 < cmin) j0 = cmin;
        if (j1 >= cmax) j1 = cmax - 1;
        for (int j = j0; j <= j1; j++) {
            int32_t lo = G_MAX(top, (int32_t)j * 256), hi = G_MIN(bot, (int32_t)(j + 1) * 256);
            int32_t cov = hi - lo;
            if (cov <= 0) continue;
            int c8 = cov >= 256 ? 255 : cov;
#if !ECCLES_GUI_HAS_AA
            c8 = c8 >= 128 ? 255 : 0;
#endif
            uint32_t o = g_opa_mul(opa, (uint32_t)c8);
            if (steep) g_px(dc, j, p, c, o);
            else g_px(dc, p, j, c, o);
        }
    }
}

static int16_t g_cos_deg(int d) { return eccles_gui_cos((uint16_t)(((uint32_t)(((d % 360) + 360) % 360) * 65536u) / 360u)); }
static int16_t g_sin_deg(int d) { return eccles_gui_sin((uint16_t)(((uint32_t)(((d % 360) + 360) % 360) * 65536u) / 360u)); }

void eccles_gui_draw_arc(eccles_gui_dc_t *dc, int cx, int cy, int r, int thickness, int a0, int a1, bool round_caps, eccles_gui_color_t c, uint8_t opa) {
    if (!dc || r <= 0 || thickness <= 0 || opa == 0) return;
    if (thickness > r) thickness = r;
    int sweep = a1 - a0;
    if (sweep <= 0) return;
    bool full = sweep >= 360;
    int32_t sx = g_cos_deg(a0), sy = g_sin_deg(a0), ex = g_cos_deg(a0 + sweep), ey = g_sin_deg(a0 + sweep);
    int ri = r - thickness;
    int32_t ro2 = 2 * r, ri2 = 2 * ri;
    int32_t kO = 261120 / ro2, kI = ri > 0 ? 261120 / ri2 : 0;
    int32_t odmax = (ro2 + 2) * (ro2 + 2);
    int32_t idmax = ri2 > 2 ? (ri2 - 2) * (ri2 - 2) : 0;
    int j0 = G_MAX(cy - r - 1, dc->clip.y1), j1 = G_MIN(cy + r + 1, dc->clip.y2 - 1);
    for (int j = j0; j <= j1; j++) {
        int32_t Y = 2 * j + 1 - 2 * cy;
        if (Y * Y > odmax) continue;
        int32_t hx = (int32_t)g_isqrt((uint32_t)(odmax - Y * Y));
        int pa = G_MAX(cx - (hx + 1) / 2 - 1, dc->clip.x1), pb = G_MIN(cx + (hx + 1) / 2, dc->clip.x2 - 1);
        int skip_a = 1, skip_b = 0;  //empty
        if (idmax > 0 && Y * Y < idmax) {
            int32_t hxi = (int32_t)g_isqrt((uint32_t)(idmax - Y * Y));
            if (hxi >= 1) { skip_a = cx - (int)((hxi + 1) / 2); skip_b = cx + (int)((hxi - 1) / 2); }
        }
        for (int px = pa; px <= pb; px++) {
            if (px >= skip_a && px <= skip_b) { px = skip_b; continue; }
            int32_t X = 2 * px + 1 - 2 * cx;
            int32_t D = X * X + Y * Y;
            int co = g_covd(D, ro2, kO);
            if (!co) continue;
            int ci = ri > 0 ? g_covd(D, ri2, kI) : 0;
            int cv = (co * (255 - ci)) / 255;
            if (!cv) continue;
            if (!full) {
                int64_t c_sp = (int64_t)sx * Y - (int64_t)sy * X;  //p is clockwise of start
                int64_t c_pe = (int64_t)X * ey - (int64_t)Y * ex;  //end is clockwise of p
                bool in;
                if (sweep <= 180) {
                    in = c_sp >= 0 && c_pe >= 0;
                } else {
                    int64_t c_ep = (int64_t)ex * Y - (int64_t)ey * X;  //p is clockwise of the end
                    int64_t c_ps = (int64_t)X * sy - (int64_t)Y * sx;  //the start is clockwise of p
                    in = !(c_ep >= 0 && c_ps >= 0);  //not inside the gap between end and start
                }
                if (!in) continue;
            }
            g_px(dc, px, j, c, g_opa_mul(opa, (uint32_t)cv));
        }
    }
    if (round_caps && !full) {
        int rm = r - thickness / 2;
        int rc = thickness / 2;
        if (rc > 0) {
            int ax = cx + (int)(((int32_t)rm * sx) / 32767), ay = cy + (int)(((int32_t)rm * sy) / 32767);
            int bx = cx + (int)(((int32_t)rm * ex) / 32767), by = cy + (int)(((int32_t)rm * ey) / 32767);
            eccles_gui_draw_rrect(dc, ax - rc, ay - rc, thickness, thickness, rc, c, opa);
            eccles_gui_draw_rrect(dc, bx - rc, by - rc, thickness, thickness, rc, c, opa);
        }
    }
}

//4. triangle rasteriser. used by eccles_gui_draw_triangle and by the 3d renderer
//pixel centers are at +0.5, edges are evaluated from the upper vertex with the same integer formula for every triangle that
//shares them, so neighbours never leave a crack and never draw a pixel twice. attributes (light, depth, uv) are planar in
//screen space: one gradient per triangle, one multiply per scanline

typedef struct { int16_t x, y; uint16_t iz; uint8_t shade; uint8_t u, v; } g_tv_t;
enum { G_R_FLAT = 0, G_R_SMOOTH = 1, G_R_TEX = 2 };
typedef struct {
    uint8_t mode, ztest, opa;
    uint16_t color;
    const uint16_t *tex;
    uint8_t tw, th;  //log2 of the texture size
} g_rp_t;

#define G_RASTER_LIM 8191

//gradient of an attribute over the triangle (a,b,c are the sorted vertices), result in Q(shift) per pixel
static void g_grad(const g_tv_t *a, const g_tv_t *b, const g_tv_t *c, int fa, int fb, int fc, int shift, int64_t area2, int32_t *gx, int32_t *gy) {
    int64_t nx = (int64_t)(fb - fa) * (c->y - a->y) - (int64_t)(fc - fa) * (b->y - a->y);
    int64_t ny = (int64_t)(fc - fa) * (b->x - a->x) - (int64_t)(fb - fa) * (c->x - a->x);
    *gx = (int32_t)((nx * ((int64_t)1 << shift)) / area2);
    *gy = (int32_t)((ny * ((int64_t)1 << shift)) / area2);
}

G_HOT static void g_raster_tri(eccles_gui_dc_t *dc, const g_tv_t *p0, const g_tv_t *p1, const g_tv_t *p2, const g_rp_t *rp) {
    g_tv_t v[3];
    v[0] = *p0; v[1] = *p1; v[2] = *p2;
    for (int i = 0; i < 3; i++)
        if (v[i].x > G_RASTER_LIM || v[i].x < -G_RASTER_LIM || v[i].y > G_RASTER_LIM || v[i].y < -G_RASTER_LIM) return;
    //sort by y
    if (v[1].y < v[0].y) { g_tv_t t = v[0]; v[0] = v[1]; v[1] = t; }
    if (v[2].y < v[1].y) { g_tv_t t = v[1]; v[1] = v[2]; v[2] = t; }
    if (v[1].y < v[0].y) { g_tv_t t = v[0]; v[0] = v[1]; v[1] = t; }
    const g_tv_t *a = &v[0], *b = &v[1], *c = &v[2];
    if (a->y == c->y) return;
    int xmin = G_MIN(a->x, G_MIN(b->x, c->x)), xmax = G_MAX(a->x, G_MAX(b->x, c->x));
    if (xmax <= dc->clip.x1 || xmin >= dc->clip.x2) return;
    int ys = G_MAX((int)a->y, (int)dc->clip.y1), ye = G_MIN((int)c->y, (int)dc->clip.y2);
    if (ys >= ye) return;
    int64_t area2 = (int64_t)(b->x - a->x) * (c->y - a->y) - (int64_t)(c->x - a->x) * (b->y - a->y);
    if (area2 == 0) return;
    int32_t s02 = (int32_t)((((int64_t)c->x - a->x) * 65536) / (c->y - a->y));
    int32_t s01 = b->y > a->y ? (int32_t)((((int64_t)b->x - a->x) * 65536) / (b->y - a->y)) : 0;
    int32_t s12 = c->y > b->y ? (int32_t)((((int64_t)c->x - b->x) * 65536) / (c->y - b->y)) : 0;
    bool smooth = rp->mode != G_R_FLAT;
    bool tex = rp->mode == G_R_TEX;
    bool zt = rp->ztest && dc->zbuf;
    int32_t shx = 0, shy = 0, ux = 0, uy = 0, vx = 0, vy = 0, zx = 0, zy = 0;
    if (smooth) g_grad(a, b, c, a->shade, b->shade, c->shade, 16, area2, &shx, &shy);
    if (tex) { g_grad(a, b, c, a->u, b->u, c->u, 16, area2, &ux, &uy); g_grad(a, b, c, a->v, b->v, c->v, 16, area2, &vx, &vy); }
    if (zt) g_grad(a, b, c, a->iz, b->iz, c->iz, 8, area2, &zx, &zy);
    uint32_t cs = g_spread(rp->color);
    int tw = 1 << rp->tw, th = 1 << rp->th;

    for (int j = ys; j < ye; j++) {
        int64_t xa = ((int64_t)a->x * 65536) + (((int64_t)s02 * (2 * (j - a->y) + 1)) >> 1);
        int64_t xb;
        if (j < b->y) xb = ((int64_t)a->x * 65536) + (((int64_t)s01 * (2 * (j - a->y) + 1)) >> 1);
        else xb = ((int64_t)b->x * 65536) + (((int64_t)s12 * (2 * (j - b->y) + 1)) >> 1);
        int64_t lo = xa < xb ? xa : xb, hi = xa < xb ? xb : xa;
        int xs = (int)((lo + 0x7FFF) >> 16), xe = (int)((hi + 0x7FFF) >> 16);
        if (xs < dc->clip.x1) xs = dc->clip.x1;
        if (xe > dc->clip.x2) xe = dc->clip.x2;
        if (xs >= xe) continue;
        if (rp->mode == G_R_FLAT && !zt) { g_span(dc, j, xs, xe, rp->color, rp->opa); continue; }
        size_t off = (size_t)(j - dc->y0) * (size_t)dc->stride + (size_t)(xs - dc->x0);
        uint16_t *px = dc->buf + off;
        uint16_t *zb = zt ? dc->zbuf + off : NULL;
        //attribute values at the center of the first pixel
        int64_t dxp = (int64_t)xs - a->x, dyp = (int64_t)j - a->y;
        int32_t sh = 0, uu = 0, vv = 0, zz = 0;
        if (smooth) sh = (int32_t)(((int64_t)a->shade << 16) + shx * dxp + shy * dyp + ((shx + shy) >> 1));
        if (tex) {
            uu = (int32_t)(((int64_t)a->u << 16) + ux * dxp + uy * dyp + ((ux + uy) >> 1));
            vv = (int32_t)(((int64_t)a->v << 16) + vx * dxp + vy * dyp + ((vx + vy) >> 1));
        }
        if (zt) zz = (int32_t)(((int64_t)a->iz << 8) + zx * dxp + zy * dyp + ((zx + zy) >> 1));
        for (int x = xs; x < xe; x++, px++) {
            if (zb) {
                int32_t z16 = zz >> 8;
                z16 = G_CLAMP(z16, 0, 65535);
                zz += zx;
                uint16_t *zp = zb++;
                if ((uint16_t)z16 <= *zp) { sh += shx; uu += ux; vv += vx; continue; }
                *zp = (uint16_t)z16;
            }
            if (rp->mode == G_R_FLAT) { *px = rp->color; continue; }
            int32_t k = sh >> 16;
            k = G_CLAMP(k, 0, 255);
            uint32_t k5 = ((uint32_t)k + 4u) >> 3;
            if (!tex) {
                *px = g_unspread((cs * k5) >> 5);
            } else {
                int32_t uc = G_CLAMP(uu, 0, (255 << 16) | 0xFFFF), vc = G_CLAMP(vv, 0, (255 << 16) | 0xFFFF);
                int tx = (int)((((uint32_t)uc >> 8) * (uint32_t)tw) >> 16);
                int ty = (int)((((uint32_t)vc >> 8) * (uint32_t)th) >> 16);
                *px = g_scale5(rp->tex[ty * tw + tx], k5);
            }
            sh += shx; uu += ux; vv += vx;
        }
    }
}

void eccles_gui_draw_triangle(eccles_gui_dc_t *dc, int x0, int y0, int x1, int y1, int x2, int y2, eccles_gui_color_t c, uint8_t opa) {
    if (!dc || opa == 0) return;
    g_tv_t a, b, d;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b); memset(&d, 0, sizeof d);
    a.x = (int16_t)G_CLAMP(x0, -G_RASTER_LIM, G_RASTER_LIM); a.y = (int16_t)G_CLAMP(y0, -G_RASTER_LIM, G_RASTER_LIM);
    b.x = (int16_t)G_CLAMP(x1, -G_RASTER_LIM, G_RASTER_LIM); b.y = (int16_t)G_CLAMP(y1, -G_RASTER_LIM, G_RASTER_LIM);
    d.x = (int16_t)G_CLAMP(x2, -G_RASTER_LIM, G_RASTER_LIM); d.y = (int16_t)G_CLAMP(y2, -G_RASTER_LIM, G_RASTER_LIM);
    g_rp_t rp;
    memset(&rp, 0, sizeof rp);
    rp.mode = G_R_FLAT; rp.opa = opa; rp.color = c;
    g_raster_tri(dc, &a, &b, &d, &rp);
}

//5. text + images

//next unicode codepoint of a utf-8 string, bad bytes give U+FFFD and advance one byte
static uint32_t g_utf8(const char **sp) {
    const uint8_t *s = (const uint8_t *)*sp;
    uint32_t c = s[0];
    if (c < 0x80u) { *sp += 1; return c; }
    if ((c & 0xE0u) == 0xC0u && (s[1] & 0xC0u) == 0x80u) { *sp += 2; return ((c & 0x1Fu) << 6) | (s[1] & 0x3Fu); }
    if ((c & 0xF0u) == 0xE0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u) {
        *sp += 3; return ((c & 0x0Fu) << 12) | ((uint32_t)(s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
    }
    if ((c & 0xF8u) == 0xF0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u && (s[3] & 0xC0u) == 0x80u) {
        *sp += 4; return ((c & 0x07u) << 18) | ((uint32_t)(s[1] & 0x3Fu) << 12) | ((uint32_t)(s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
    }
    *sp += 1;
    return 0xFFFDu;
}

static const eccles_gui_glyph_t *g_glyph_raw(const eccles_gui_font_t *f, uint32_t cp) {
    for (int i = 0; i < f->nranges; i++) {
        const eccles_gui_range_t *r = &f->ranges[i];
        if (cp >= r->first && cp < r->first + r->count) return &f->glyphs[r->index + (cp - r->first)];
    }
    return NULL;
}
static const eccles_gui_glyph_t *g_glyph(const eccles_gui_font_t *f, uint32_t cp) {
    const eccles_gui_glyph_t *g = g_glyph_raw(f, cp);
    return g ? g : g_glyph_raw(f, '?');
}
static const eccles_gui_font_t *g_font(const eccles_gui_t *gui, uint8_t id, uint8_t *scale) {
    if (id >= gui->nfonts || !gui->fonts[id].f) { *scale = 1; return NULL; }
    *scale = gui->fonts[id].scale;
    return gui->fonts[id].f;
}

int eccles_gui_font_height(const eccles_gui_t *gui, uint8_t font) {
    uint8_t sc;
    const eccles_gui_font_t *f = gui ? g_font(gui, font, &sc) : NULL;
    return f ? f->line_height * sc : 0;
}

//finds the end of the line that starts at s. returns where the next line starts
static const char *g_line(const eccles_gui_font_t *f, uint8_t sc, const char *s, int max_w, bool wrap, int *len, int *wpx) {
    const char *p = s, *last_space = NULL;
    int w = 0, w_space = 0;
    while (*p && *p != '\n') {
        const char *q = p;
        uint32_t cp = g_utf8(&q);
        const eccles_gui_glyph_t *g = g_glyph(f, cp);
        int adv = g ? g->adv * sc : 0;
        if (wrap && max_w > 0 && w + adv > max_w && p > s) {
            if (last_space) { *len = (int)(last_space - s); *wpx = w_space; return last_space + 1; }
            *len = (int)(p - s); *wpx = w; return p;
        }
        if (cp == ' ') { last_space = p; w_space = w; }
        w += adv;
        p = q;
    }
    *len = (int)(p - s);
    *wpx = w;
    return *p == '\n' ? p + 1 : p;
}

int eccles_gui_text_width(const eccles_gui_t *gui, uint8_t font, const char *text) {
    uint8_t sc;
    const eccles_gui_font_t *f = (gui && text) ? g_font(gui, font, &sc) : NULL;
    if (!f) return 0;
    int len, w;
    g_line(f, sc, text, 0, false, &len, &w);
    return w;
}

int eccles_gui_text_height(const eccles_gui_t *gui, uint8_t font, const char *text, int max_w) {
    uint8_t sc;
    const eccles_gui_font_t *f = (gui && text) ? g_font(gui, font, &sc) : NULL;
    if (!f) return 0;
    int lines = 0;
    while (*text) {
        int len, w;
        text = g_line(f, sc, text, max_w, max_w > 0, &len, &w);
        lines++;
    }
    return lines * f->line_height * sc;
}

G_HOT static void g_draw_glyph(eccles_gui_dc_t *dc, const eccles_gui_font_t *f, int sc, const eccles_gui_glyph_t *g, int penx, int topy, uint16_t c, uint32_t opa) {
    if (g->w == 0 || g->h == 0) return;
    int gx = penx + g->ox * sc, gy = topy + g->oy * sc;
    int w = g->w, h = g->h;
    if (gx >= dc->clip.x2 || gx + w * sc <= dc->clip.x1 || gy >= dc->clip.y2 || gy + h * sc <= dc->clip.y1) return;
    const uint8_t *bm = f->bitmap + g->off;
    int rb = f->bpp == 1 ? (w + 7) / 8 : (f->bpp == 4 ? (w + 1) / 2 : w);
    for (int r = 0; r < h; r++) {
        int ry = gy + r * sc;
        if (ry + sc <= dc->clip.y1 || ry >= dc->clip.y2) continue;
        const uint8_t *row = bm + (size_t)r * (size_t)rb;
        if (f->bpp == 1) {
            int col = 0;
            while (col < w) {
                if (row[col >> 3] & (0x80u >> (col & 7))) {
                    int start = col;
                    while (col < w && (row[col >> 3] & (0x80u >> (col & 7)))) col++;
                    for (int t = 0; t < sc; t++) g_span(dc, ry + t, gx + start * sc, gx + col * sc, c, opa);
                } else col++;
            }
        } else {
            for (int col = 0; col < w; col++) {
                uint32_t a = f->bpp == 8 ? row[col] : ((col & 1) ? (row[col >> 1] & 0x0Fu) : (row[col >> 1] >> 4)) * 17u;
                if (!a) continue;
                uint32_t o = g_opa_mul(opa, a);
                for (int t = 0; t < sc; t++) g_span(dc, ry + t, gx + col * sc, gx + (col + 1) * sc, c, o);
            }
        }
    }
}

//draws `len` bytes of text starting at x. returns the pen x afterwards
static int g_draw_run(eccles_gui_dc_t *dc, const eccles_gui_font_t *f, int sc, int x, int y, const char *s, int len, uint16_t c, uint32_t opa) {
    const char *end = s + len;
    while (s < end) {
        uint32_t cp = g_utf8(&s);
        const eccles_gui_glyph_t *g = g_glyph(f, cp);
        if (!g) continue;
        if (x < dc->clip.x2) g_draw_glyph(dc, f, sc, g, x, y, c, opa);
        x += g->adv * sc;
    }
    return x;
}

int eccles_gui_draw_text(eccles_gui_dc_t *dc, int x, int y, uint8_t font, const char *text, eccles_gui_color_t c, uint8_t opa) {
    uint8_t sc;
    const eccles_gui_font_t *f = (dc && text) ? g_font(dc->gui, font, &sc) : NULL;
    if (!f || opa == 0) return 0;
    int widest = 0;
    while (*text) {
        int len, w;
        const char *next = g_line(f, sc, text, 0, false, &len, &w);
        if (y < dc->clip.y2 && y + f->line_height * sc > dc->clip.y1) g_draw_run(dc, f, sc, x, y, text, len, c, opa);
        if (w > widest) widest = w;
        y += f->line_height * sc;
        text = next;
        if (y >= dc->clip.y2) break;
    }
    return widest;
}

void eccles_gui_draw_text_box(eccles_gui_dc_t *dc, const eccles_gui_area_t *box, uint8_t font, const char *text, eccles_gui_color_t c, uint8_t opa, uint8_t mode) {
    uint8_t sc;
    const eccles_gui_font_t *f = (dc && box && text) ? g_font(dc->gui, font, &sc) : NULL;
    if (!f || opa == 0 || g_area_empty(box)) return;
    eccles_gui_area_t saved = dc->clip;
    dc->clip = g_area_isect(&saved, box);
    if (!g_area_empty(&dc->clip)) {
        int bw = box->x2 - box->x1, lh = f->line_height * sc;
        bool wrap = (mode & ECCLES_GUI_TEXT_WRAP) != 0;
        int y = box->y1;
        while (*text && y < dc->clip.y2) {
            int len, w;
            const char *next = g_line(f, sc, text, bw, wrap, &len, &w);
            if (y + lh > dc->clip.y1) {
                bool ell = !wrap && (mode & ECCLES_GUI_TEXT_ELLIPSIS) && w > bw;
                const eccles_gui_glyph_t *dot = g_glyph(f, '.');
                int dots_w = dot ? 3 * dot->adv * sc : 0;
                int draw_len = len, draw_w = w;
                if (ell) {  //longest prefix that leaves room for "..."
                    const char *p = text, *end = text + len;
                    int pw = 0;
                    draw_len = 0;
                    while (p < end) {
                        const char *q = p;
                        const eccles_gui_glyph_t *g = g_glyph(f, g_utf8(&q));
                        int adv = g ? g->adv * sc : 0;
                        if (pw + adv + dots_w > bw) break;
                        pw += adv; p = q;
                    }
                    draw_len = (int)(p - text);
                    draw_w = pw + dots_w;
                }
                int ax = box->x1;
                if ((mode & 3) == ECCLES_GUI_TEXT_CENTER) ax += (bw - draw_w) / 2;
                else if ((mode & 3) == ECCLES_GUI_TEXT_RIGHT) ax += bw - draw_w;
                int px = g_draw_run(dc, f, sc, ax, y, text, draw_len, c, opa);
                if (ell && dot) for (int i = 0; i < 3; i++) { g_draw_glyph(dc, f, sc, dot, px, y, c, opa); px += dot->adv * sc; }
            }
            y += lh;
            text = next;
        }
    }
    dc->clip = saved;
}

void eccles_gui_draw_image(eccles_gui_dc_t *dc, int x, int y, const eccles_gui_image_t *img, eccles_gui_color_t c, uint8_t opa) {
    if (!dc || !img || !img->data || img->w == 0 || img->h == 0 || opa == 0) return;
    int w = img->w, h = img->h;
    int xa = G_MAX(x, (int)dc->clip.x1), xb = G_MIN(x + w, (int)dc->clip.x2);
    int j0 = G_MAX(y, (int)dc->clip.y1), j1 = G_MIN(y + h, (int)dc->clip.y2);
    if (xa >= xb || j0 >= j1) return;
    const uint8_t *data = (const uint8_t *)img->data;
    uint32_t cs = g_spread(c);
    for (int j = j0; j < j1; j++) {
        uint16_t *dst = dc->buf + (size_t)(j - dc->y0) * (size_t)dc->stride + (size_t)(xa - dc->x0);
        int r = j - y;
        switch (img->fmt) {
        case ECCLES_GUI_IMG_RGB565: case ECCLES_GUI_IMG_RGB565_KEY: {
            const uint16_t *src = (const uint16_t *)data + (size_t)r * (size_t)w + (size_t)(xa - x);
            bool key = img->fmt == ECCLES_GUI_IMG_RGB565_KEY;
            if (opa >= 255 && !key) { memcpy(dst, src, (size_t)(xb - xa) * 2u); break; }
            for (int i = xa; i < xb; i++, dst++, src++) {
                if (key && *src == img->key) continue;
                *dst = opa >= 255 ? *src : g_blend(*dst, *src, opa);
            }
            break;
        }
        case ECCLES_GUI_IMG_A8: {
            const uint8_t *src = data + (size_t)r * (size_t)w + (size_t)(xa - x);
            for (int i = xa; i < xb; i++, dst++, src++) {
                uint32_t a = opa >= 255 ? *src : g_opa_mul(*src, opa);
                if (a) *dst = a >= 255 ? c : g_blend_s(*dst, cs, g_a5(a));
            }
            break;
        }
        case ECCLES_GUI_IMG_A4: {
            const uint8_t *row = data + (size_t)r * (size_t)((w + 1) / 2);
            for (int i = xa; i < xb; i++, dst++) {
                int col = i - x;
                uint32_t a = ((col & 1) ? (row[col >> 1] & 0x0Fu) : (row[col >> 1] >> 4)) * 17u;
                a = opa >= 255 ? a : g_opa_mul(a, opa);
                if (a) *dst = a >= 255 ? c : g_blend_s(*dst, cs, g_a5(a));
            }
            break;
        }
        case ECCLES_GUI_IMG_A1: {
            const uint8_t *row = data + (size_t)r * (size_t)((w + 7) / 8);
            for (int i = xa; i < xb; i++, dst++) {
                int col = i - x;
                if (row[col >> 3] & (0x80u >> (col & 7))) *dst = opa >= 255 ? c : g_blend_s(*dst, cs, g_a5(opa));
            }
            break;
        }
        default: return;
        }
    }
}

//6. widgets

#if ECCLES_GUI_HAS_WIDGETS

const eccles_gui_palette_t eccles_gui_palette_dark = {
    ECCLES_GUI_HEX(0x10141A), ECCLES_GUI_HEX(0x1B222C), ECCLES_GUI_HEX(0x263040), ECCLES_GUI_HEX(0x344055),
    ECCLES_GUI_HEX(0xE8EDF3), ECCLES_GUI_HEX(0x8A96A6), ECCLES_GUI_HEX(0x3D8BFF), ECCLES_GUI_HEX(0xFFFFFF)
};
const eccles_gui_palette_t eccles_gui_palette_light = {
    ECCLES_GUI_HEX(0xF2F4F8), ECCLES_GUI_HEX(0xFFFFFF), ECCLES_GUI_HEX(0xE6EAF0), ECCLES_GUI_HEX(0xC9D0DA),
    ECCLES_GUI_HEX(0x1A2230), ECCLES_GUI_HEX(0x6B778A), ECCLES_GUI_HEX(0x2F6FED), ECCLES_GUI_HEX(0xFFFFFF)
};

#define G_AUTO 0x10u  //aux bit: label/button/checkbox resize themselves to fit their text
#define G_TEXT_MODE_MASK 0x0Fu

static inline bool g_ok(const eccles_gui_t *gui, eccles_gui_obj_t o) {
    return gui && gui->ready && o < ECCLES_GUI_MAX_WIDGETS && gui->w[o].type != ECCLES_GUI_W_FREE;
}
static bool g_ok_t(eccles_gui_t *gui, eccles_gui_obj_t o, int type) {
    if (!g_ok(gui, o)) { g_err(gui, ECCLES_GUI_ERR_BAD_OBJECT, "obj"); return false; }
    if (gui->w[o].type != type) { g_err(gui, ECCLES_GUI_ERR_BAD_TYPE, "type"); return false; }
    return true;
}

//position of w inside its parent
static void g_rel(const eccles_gui_t *gui, const eccles_gui_widget_t *w, int *rx, int *ry) {
    int x = w->x, y = w->y;
    if (w->parent != ECCLES_GUI_NO_OBJ && w->align != ECCLES_GUI_ALIGN_NONE) {
        const eccles_gui_widget_t *p = &gui->w[w->parent];
        int dw = p->w - w->w, dh = p->h - w->h;
        switch (w->align) {
        case ECCLES_GUI_ALIGN_TOP_MID: x += dw / 2; break;
        case ECCLES_GUI_ALIGN_TOP_RIGHT: x += dw; break;
        case ECCLES_GUI_ALIGN_MID_LEFT: y += dh / 2; break;
        case ECCLES_GUI_ALIGN_CENTER: x += dw / 2; y += dh / 2; break;
        case ECCLES_GUI_ALIGN_MID_RIGHT: x += dw; y += dh / 2; break;
        case ECCLES_GUI_ALIGN_BOTTOM_LEFT: y += dh; break;
        case ECCLES_GUI_ALIGN_BOTTOM_MID: x += dw / 2; y += dh; break;
        case ECCLES_GUI_ALIGN_BOTTOM_RIGHT: x += dw; y += dh; break;
        default: break;
        }
    }
    *rx = x; *ry = y;
}

//absolute position of a widget (walks up to the root)
static void g_abs(const eccles_gui_t *gui, eccles_gui_obj_t o, int *ax, int *ay) {
    eccles_gui_obj_t chain[ECCLES_GUI_MAX_WIDGETS > 64 ? 64 : ECCLES_GUI_MAX_WIDGETS];
    int n = 0;
    while (o != ECCLES_GUI_NO_OBJ && n < (int)(sizeof chain / sizeof chain[0])) { chain[n++] = o; o = gui->w[o].parent; }
    int x = 0, y = 0;
    for (int i = n - 1; i >= 0; i--) {
        int rx, ry;
        g_rel(gui, &gui->w[chain[i]], &rx, &ry);
        x += rx; y += ry;
    }
    *ax = x; *ay = y;
}

static eccles_gui_area_t g_obj_area(const eccles_gui_t *gui, eccles_gui_obj_t o) {
    int x, y;
    g_abs(gui, o, &x, &y);
    return g_area_make(x, y, gui->w[o].w, gui->w[o].h);
}

eccles_gui_area_t eccles_gui_obj_get_area(const eccles_gui_t *gui, eccles_gui_obj_t obj) {
    if (!g_ok(gui, obj)) return g_area_make(0, 0, 0, 0);
    return g_obj_area(gui, obj);
}

static void g_inv(eccles_gui_t *gui, eccles_gui_obj_t o) {
    eccles_gui_area_t a = g_obj_area(gui, o);
    int sh = gui->styles[gui->w[o].style].shadow;
    int m = sh ? sh + sh / 3 + 2 : 0;
    a.x1 = (int16_t)(a.x1 - m); a.y1 = (int16_t)(a.y1 - m); a.x2 = (int16_t)(a.x2 + m); a.y2 = (int16_t)(a.y2 + m);
    g_dirty_add(gui, a);
}
static void g_autosize(eccles_gui_t *gui, eccles_gui_obj_t o);
void eccles_gui_obj_invalidate(eccles_gui_t *gui, eccles_gui_obj_t obj) { if (g_ok(gui, obj)) g_inv(gui, obj); }

//styles + theme

eccles_gui_status_t eccles_gui_style_set(eccles_gui_t *gui, uint8_t slot, const eccles_gui_style_t *style) {
    if (!gui || !gui->ready || !style || slot >= ECCLES_GUI_MAX_STYLES) { g_err(gui, ECCLES_GUI_ERR_BAD_ARG, "style"); return ECCLES_GUI_FAILED; }
    gui->styles[slot] = *style;
    if (slot >= gui->nstyles) gui->nstyles = (uint8_t)(slot + 1);
    eccles_gui_invalidate_all(gui);
    return ECCLES_GUI_SUCCESSFUL;
}
const eccles_gui_style_t *eccles_gui_style_get(const eccles_gui_t *gui, uint8_t slot) {
    return (gui && slot < ECCLES_GUI_MAX_STYLES) ? &gui->styles[slot] : NULL;
}
int eccles_gui_style_add(eccles_gui_t *gui, const eccles_gui_style_t *style) {
    if (!gui || !gui->ready || !style) { g_err(gui, ECCLES_GUI_ERR_BAD_ARG, "style"); return -1; }
    if (gui->nstyles >= ECCLES_GUI_MAX_STYLES) { g_err(gui, ECCLES_GUI_ERR_NO_MEMORY, "styles"); return -1; }
    int slot = gui->nstyles++;
    gui->styles[slot] = *style;
    return slot;
}

void eccles_gui_theme_apply(eccles_gui_t *gui, const eccles_gui_palette_t *p) {
    if (!gui || !p) return;
    int u = gui->d.width < 300 ? 1 : (gui->d.width < 600 ? 2 : 3);  //1x, 2x or 3x sized ui
    uint8_t fl = (uint8_t)(u - 1), ft = (uint8_t)u;
    eccles_gui_style_t s;
    memset(&s, 0, sizeof s);
    s.bg = s.bg2 = p->bg; s.text = p->text; s.border = p->border; s.accent = p->accent; s.opa = 255; s.font = fl;
    gui->styles[ECCLES_GUI_STYLE_SCREEN] = s;
    s.opa = 0;
    gui->styles[ECCLES_GUI_STYLE_PANEL] = s;
    //card
    memset(&s, 0, sizeof s);
    s.bg = p->surface; s.bg2 = p->surface2; s.border = p->border; s.text = p->text; s.accent = p->accent;
    s.border_w = 1; s.radius = (uint8_t)(3 + 3 * u); s.pad = (uint8_t)(2 + 2 * u); s.shadow = (uint8_t)(2 * u + 2); s.opa = 255; s.font = fl;
    gui->styles[ECCLES_GUI_STYLE_CARD] = s;
    //button
    s.bg = p->surface2; s.bg2 = eccles_gui_color_mix(p->surface2, p->bg, 110); s.grad = ECCLES_GUI_GRAD_V;
    s.radius = (uint8_t)(2 + 3 * u); s.pad = (uint8_t)(2 + 2 * u); s.shadow = 0;
    gui->styles[ECCLES_GUI_STYLE_BUTTON] = s;
    s.bg = p->accent; s.bg2 = eccles_gui_color_mix(p->accent, 0, 70); s.border_w = 0; s.text = p->on_accent;
    gui->styles[ECCLES_GUI_STYLE_BUTTON_PRIMARY] = s;
    //labels
    memset(&s, 0, sizeof s);
    s.text = p->text; s.accent = p->accent; s.font = fl;
    gui->styles[ECCLES_GUI_STYLE_LABEL] = s;
    s.font = ft;
    gui->styles[ECCLES_GUI_STYLE_LABEL_TITLE] = s;
    s.font = fl; s.text = p->muted;
    gui->styles[ECCLES_GUI_STYLE_LABEL_MUTED] = s;
    if (gui->nstyles < ECCLES_GUI_STYLE_USER) gui->nstyles = ECCLES_GUI_STYLE_USER;
    eccles_gui_invalidate_all(gui);
}

//the tree

eccles_gui_obj_t eccles_gui_root(const eccles_gui_t *gui) { (void)gui; return 0; }
bool eccles_gui_obj_valid(const eccles_gui_t *gui, eccles_gui_obj_t obj) { return g_ok(gui, obj); }
eccles_gui_obj_t eccles_gui_obj_parent(const eccles_gui_t *gui, eccles_gui_obj_t obj) { return g_ok(gui, obj) ? gui->w[obj].parent : ECCLES_GUI_NO_OBJ; }
eccles_gui_obj_t eccles_gui_obj_child(const eccles_gui_t *gui, eccles_gui_obj_t obj) { return g_ok(gui, obj) ? gui->w[obj].child : ECCLES_GUI_NO_OBJ; }
eccles_gui_obj_t eccles_gui_obj_next(const eccles_gui_t *gui, eccles_gui_obj_t obj) { return g_ok(gui, obj) ? gui->w[obj].next : ECCLES_GUI_NO_OBJ; }

static void g_unlink(eccles_gui_t *gui, eccles_gui_obj_t o) {
    eccles_gui_widget_t *w = &gui->w[o];
    if (w->parent == ECCLES_GUI_NO_OBJ) return;
    eccles_gui_widget_t *p = &gui->w[w->parent];
    if (p->child == o) p->child = w->next;
    else {
        for (eccles_gui_obj_t c = p->child; c != ECCLES_GUI_NO_OBJ; c = gui->w[c].next)
            if (gui->w[c].next == o) { gui->w[c].next = w->next; break; }
    }
    w->next = ECCLES_GUI_NO_OBJ;
}
static void g_link_front(eccles_gui_t *gui, eccles_gui_obj_t parent, eccles_gui_obj_t o) {  //top of the stack = end of the list
    eccles_gui_widget_t *p = &gui->w[parent];
    gui->w[o].parent = parent;
    gui->w[o].next = ECCLES_GUI_NO_OBJ;
    if (p->child == ECCLES_GUI_NO_OBJ) { p->child = o; return; }
    eccles_gui_obj_t c = p->child;
    while (gui->w[c].next != ECCLES_GUI_NO_OBJ) c = gui->w[c].next;
    gui->w[c].next = o;
}

static eccles_gui_obj_t g_alloc(eccles_gui_t *gui, uint8_t type, eccles_gui_obj_t parent, uint8_t style, uint8_t flags) {
    if (!g_ok(gui, parent)) { g_err(gui, ECCLES_GUI_ERR_BAD_OBJECT, "parent"); return ECCLES_GUI_NO_OBJ; }
    eccles_gui_obj_t i = 1;
    while (i < ECCLES_GUI_MAX_WIDGETS && gui->w[i].type != ECCLES_GUI_W_FREE) i++;
    if (i >= ECCLES_GUI_MAX_WIDGETS) { g_err(gui, ECCLES_GUI_ERR_NO_MEMORY, "widgets"); return ECCLES_GUI_NO_OBJ; }
    eccles_gui_widget_t *w = &gui->w[i];
    memset(w, 0, sizeof *w);
    w->type = type; w->style = style; w->flags = flags; w->opa = 255;
    w->child = w->next = ECCLES_GUI_NO_OBJ;
    w->w = 100; w->h = 40;
    g_link_front(gui, parent, i);
    gui->nwidgets++;
    gui->stats.widgets_used = gui->nwidgets;
    if (gui->nwidgets > gui->stats.widgets_peak) gui->stats.widgets_peak = gui->nwidgets;
    return i;
}

#if ECCLES_GUI_HAS_3D && ECCLES_GUI_MAX_VIEWS3D > 0
static void g_view_release(eccles_gui_t *gui, eccles_gui_widget_t *w) {
    if (w->type == ECCLES_GUI_W_VIEW3D && w->u.v3d.view < ECCLES_GUI_MAX_VIEWS3D) gui->view_used[w->u.v3d.view] = 0;
}
#endif

static void g_free_tree(eccles_gui_t *gui, eccles_gui_obj_t o) {
    eccles_gui_obj_t c = gui->w[o].child;
    while (c != ECCLES_GUI_NO_OBJ) { eccles_gui_obj_t n = gui->w[c].next; g_free_tree(gui, c); c = n; }
#if ECCLES_GUI_HAS_ANIM && ECCLES_GUI_MAX_ANIMS > 0
    eccles_gui_anim_cancel(gui, o, 0);
#endif
#if ECCLES_GUI_HAS_3D && ECCLES_GUI_MAX_VIEWS3D > 0
    g_view_release(gui, &gui->w[o]);
#endif
    if (gui->t_obj == o) gui->t_obj = ECCLES_GUI_NO_OBJ;
    if (gui->fling_obj == o) gui->fling_obj = ECCLES_GUI_NO_OBJ;
    memset(&gui->w[o], 0, sizeof gui->w[o]);  //type 0 = free
    gui->nwidgets--;
}

eccles_gui_status_t eccles_gui_obj_delete(eccles_gui_t *gui, eccles_gui_obj_t obj) {
    if (!g_ok(gui, obj) || obj == 0) { g_err(gui, ECCLES_GUI_ERR_BAD_OBJECT, "obj"); return ECCLES_GUI_FAILED; }
    g_inv(gui, obj);
    g_unlink(gui, obj);
    g_free_tree(gui, obj);
    gui->stats.widgets_used = gui->nwidgets;
    return ECCLES_GUI_SUCCESSFUL;
}

eccles_gui_status_t eccles_gui_obj_set_parent(eccles_gui_t *gui, eccles_gui_obj_t obj, eccles_gui_obj_t parent) {
    if (!g_ok(gui, obj) || !g_ok(gui, parent) || obj == 0) { g_err(gui, ECCLES_GUI_ERR_BAD_OBJECT, "obj"); return ECCLES_GUI_FAILED; }
    for (eccles_gui_obj_t p = parent; p != ECCLES_GUI_NO_OBJ; p = gui->w[p].parent)
        if (p == obj) { g_err(gui, ECCLES_GUI_ERR_BAD_ARG, "cycle"); return ECCLES_GUI_FAILED; }
    g_inv(gui, obj);
    g_unlink(gui, obj);
    g_link_front(gui, parent, obj);
    g_inv(gui, obj);
    return ECCLES_GUI_SUCCESSFUL;
}

void eccles_gui_obj_to_front(eccles_gui_t *gui, eccles_gui_obj_t obj) {
    if (!g_ok(gui, obj) || obj == 0) return;
    eccles_gui_obj_t p = gui->w[obj].parent;
    g_unlink(gui, obj);
    g_link_front(gui, p, obj);
    g_inv(gui, obj);
}
void eccles_gui_obj_to_back(eccles_gui_t *gui, eccles_gui_obj_t obj) {
    if (!g_ok(gui, obj) || obj == 0) return;
    eccles_gui_obj_t p = gui->w[obj].parent;
    g_unlink(gui, obj);
    gui->w[obj].next = gui->w[p].child;
    gui->w[p].child = obj;
    g_inv(gui, obj);
}

//topmost (deepest, last drawn) widget under a point
static eccles_gui_obj_t g_hit(const eccles_gui_t *gui, eccles_gui_obj_t o, int px, int py, int x, int y, bool clickable) {
    const eccles_gui_widget_t *w = &gui->w[o];
    if (w->flags & ECCLES_GUI_F_HIDDEN) return ECCLES_GUI_NO_OBJ;
    int rx, ry;
    g_rel(gui, w, &rx, &ry);
    int ax = px + rx, ay = py + ry;
    if (x < ax || y < ay || x >= ax + w->w || y >= ay + w->h) return ECCLES_GUI_NO_OBJ;
    eccles_gui_obj_t best = ECCLES_GUI_NO_OBJ;
    for (eccles_gui_obj_t c = w->child; c != ECCLES_GUI_NO_OBJ; c = gui->w[c].next) {
        eccles_gui_obj_t r = g_hit(gui, c, ax, ay, x, y, clickable);
        if (r != ECCLES_GUI_NO_OBJ) best = r;
    }
    if (best != ECCLES_GUI_NO_OBJ) return best;
    if (!clickable || ((w->flags & ECCLES_GUI_F_CLICKABLE) && !(w->flags & ECCLES_GUI_F_DISABLED))) return o;
    return ECCLES_GUI_NO_OBJ;
}
eccles_gui_obj_t eccles_gui_obj_at(const eccles_gui_t *gui, int x, int y, bool only_clickable) {
    if (!gui || !gui->ready) return ECCLES_GUI_NO_OBJ;
    return g_hit(gui, 0, 0, 0, x, y, only_clickable);
}

//geometry + look setters

//the root is the whole panel: it cannot be moved, resized, aligned or hidden
static bool g_root_guard(eccles_gui_t *gui, eccles_gui_obj_t o) {
    if (o != 0) return false;
    g_err(gui, ECCLES_GUI_ERR_BAD_ARG, "root");
    return true;
}

void eccles_gui_obj_set_area(eccles_gui_t *gui, eccles_gui_obj_t obj, int x, int y, int w, int h) {
    if (!g_ok(gui, obj)) { g_err(gui, ECCLES_GUI_ERR_BAD_OBJECT, "obj"); return; }
    if (g_root_guard(gui, obj)) return;
    eccles_gui_widget_t *o = &gui->w[obj];
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    g_inv(gui, obj);
    o->x = (int16_t)x; o->y = (int16_t)y; o->w = (int16_t)w; o->h = (int16_t)h;
    o->aux = (uint8_t)(o->aux & ~G_AUTO);
    g_inv(gui, obj);
}
void eccles_gui_obj_set_pos(eccles_gui_t *gui, eccles_gui_obj_t obj, int x, int y) {
    if (!g_ok(gui, obj)) { g_err(gui, ECCLES_GUI_ERR_BAD_OBJECT, "obj"); return; }
    if (g_root_guard(gui, obj)) return;
    eccles_gui_widget_t *o = &gui->w[obj];
    g_inv(gui, obj);
    o->x = (int16_t)x; o->y = (int16_t)y;
    g_inv(gui, obj);
}
void eccles_gui_obj_set_size(eccles_gui_t *gui, eccles_gui_obj_t obj, int w, int h) {
    if (!g_ok(gui, obj)) { g_err(gui, ECCLES_GUI_ERR_BAD_OBJECT, "obj"); return; }
    eccles_gui_obj_set_area(gui, obj, gui->w[obj].x, gui->w[obj].y, w, h);
}
void eccles_gui_obj_set_align(eccles_gui_t *gui, eccles_gui_obj_t obj, eccles_gui_align_t align, int ox, int oy) {
    if (!g_ok(gui, obj)) { g_err(gui, ECCLES_GUI_ERR_BAD_OBJECT, "obj"); return; }
    if (g_root_guard(gui, obj)) return;
    g_inv(gui, obj);
    gui->w[obj].align = (uint8_t)align; gui->w[obj].x = (int16_t)ox; gui->w[obj].y = (int16_t)oy;
    g_inv(gui, obj);
}
void eccles_gui_obj_set_style(eccles_gui_t *gui, eccles_gui_obj_t obj, uint8_t style) {
    if (!g_ok(gui, obj) || style >= ECCLES_GUI_MAX_STYLES) { g_err(gui, ECCLES_GUI_ERR_BAD_ARG, "style"); return; }
    g_inv(gui, obj);
    gui->w[obj].style = style;
    g_autosize(gui, obj);  //the font may have changed
    g_inv(gui, obj);
}
void eccles_gui_obj_set_opa(eccles_gui_t *gui, eccles_gui_obj_t obj, uint8_t opa) {
    if (!g_ok(gui, obj)) { g_err(gui, ECCLES_GUI_ERR_BAD_OBJECT, "obj"); return; }
    if (gui->w[obj].opa == opa) return;
    gui->w[obj].opa = opa;
    g_inv(gui, obj);
}
static void g_set_flag(eccles_gui_t *gui, eccles_gui_obj_t obj, uint8_t flag, bool on) {
    if (!g_ok(gui, obj)) { g_err(gui, ECCLES_GUI_ERR_BAD_OBJECT, "obj"); return; }
    if (flag == ECCLES_GUI_F_HIDDEN && g_root_guard(gui, obj)) return;
    uint8_t before = gui->w[obj].flags;
    gui->w[obj].flags = (uint8_t)(on ? (before | flag) : (before & ~flag));
    if (gui->w[obj].flags != before) g_inv(gui, obj);
}
void eccles_gui_obj_set_hidden(eccles_gui_t *gui, eccles_gui_obj_t obj, bool hidden) { g_set_flag(gui, obj, ECCLES_GUI_F_HIDDEN, hidden); }
void eccles_gui_obj_set_enabled(eccles_gui_t *gui, eccles_gui_obj_t obj, bool enabled) { g_set_flag(gui, obj, ECCLES_GUI_F_DISABLED, !enabled); }
void eccles_gui_obj_set_clickable(eccles_gui_t *gui, eccles_gui_obj_t obj, bool clickable) { g_set_flag(gui, obj, ECCLES_GUI_F_CLICKABLE, clickable); }
void eccles_gui_obj_set_user(eccles_gui_t *gui, eccles_gui_obj_t obj, void *user) { if (g_ok(gui, obj)) gui->w[obj].user = user; }
void *eccles_gui_obj_get_user(const eccles_gui_t *gui, eccles_gui_obj_t obj) { return g_ok(gui, obj) ? gui->w[obj].user : NULL; }
void eccles_gui_obj_on(eccles_gui_t *gui, eccles_gui_obj_t obj, eccles_gui_event_fn cb, void *user) {
    if (!g_ok(gui, obj)) { g_err(gui, ECCLES_GUI_ERR_BAD_OBJECT, "obj"); return; }
    gui->w[obj].cb = cb;
    gui->w[obj].user = user;
}

eccles_gui_status_t eccles_gui_obj_layout(eccles_gui_t *gui, eccles_gui_obj_t obj, int direction, int pad, int gap) {
    if (!g_ok(gui, obj)) { g_err(gui, ECCLES_GUI_ERR_BAD_OBJECT, "obj"); return ECCLES_GUI_FAILED; }
    bool row = direction == ECCLES_GUI_LAYOUT_ROW;
    eccles_gui_widget_t *p = &gui->w[obj];
    int n = 0, fixed = 0, grow = 0;
    for (eccles_gui_obj_t c = p->child; c != ECCLES_GUI_NO_OBJ; c = gui->w[c].next) {
        if (gui->w[c].flags & ECCLES_GUI_F_HIDDEN) continue;
        int m = row ? gui->w[c].w : gui->w[c].h;
        n++;
        if (m == 0) grow++; else fixed += m;
    }
    if (!n) return ECCLES_GUI_SUCCESSFUL;
    int main_total = row ? p->w : p->h, cross_total = row ? p->h : p->w;
    int avail = main_total - 2 * pad - gap * (n - 1) - fixed;
    int share = grow ? (avail > 0 ? avail / grow : 0) : 0;
    int extra = grow ? (avail > 0 ? avail - share * grow : 0) : 0;  //the rounding leftovers go to the first ones
    int pos = pad;
    for (eccles_gui_obj_t c = p->child; c != ECCLES_GUI_NO_OBJ; c = gui->w[c].next) {
        eccles_gui_widget_t *w = &gui->w[c];
        if (w->flags & ECCLES_GUI_F_HIDDEN) continue;
        int m = row ? w->w : w->h, x2 = row ? w->h : w->w;
        if (m == 0) { m = share + (extra > 0 ? 1 : 0); if (extra > 0) extra--; }
        if (x2 == 0) x2 = cross_total - 2 * pad;
        int cross = (cross_total - x2) / 2;
        g_inv(gui, c);
        w->align = ECCLES_GUI_ALIGN_NONE;
        if (row) { w->x = (int16_t)pos; w->y = (int16_t)cross; w->w = (int16_t)m; w->h = (int16_t)x2; }
        else { w->y = (int16_t)pos; w->x = (int16_t)cross; w->h = (int16_t)m; w->w = (int16_t)x2; }
        w->aux = (uint8_t)(w->aux & ~G_AUTO);
        g_inv(gui, c);
        pos += m + gap;
    }
    return ECCLES_GUI_SUCCESSFUL;
}

//creating

static void g_autosize(eccles_gui_t *gui, eccles_gui_obj_t o) {
    eccles_gui_widget_t *w = &gui->w[o];
    if (!(w->aux & G_AUTO)) return;
    const eccles_gui_style_t *s = &gui->styles[w->style];
    const char *t = w->u.label.text ? w->u.label.text : "";
    int tw = eccles_gui_text_width(gui, s->font, t);
    int fh = eccles_gui_font_height(gui, s->font);
    int nw = w->w, nh = w->h;
    if (w->type == ECCLES_GUI_W_LABEL) {
        if (w->aux & ECCLES_GUI_TEXT_WRAP) nh = eccles_gui_text_height(gui, s->font, t, w->w - 2 * s->pad) + 2 * s->pad;
        else { nw = tw + 2 * s->pad; nh = eccles_gui_text_height(gui, s->font, t, 0) + 2 * s->pad; }
    } else if (w->type == ECCLES_GUI_W_BUTTON) {
        nw = tw + 4 * s->pad; nh = eccles_gui_text_height(gui, s->font, t, 0) + 3 * s->pad;
    } else if (w->type == ECCLES_GUI_W_CHECKBOX) {
        int bs = fh + 6;
        nw = bs + s->pad + tw; nh = bs;
    }
    if (nw != w->w || nh != w->h) { g_inv(gui, o); w->w = (int16_t)nw; w->h = (int16_t)nh; g_inv(gui, o); }
}

eccles_gui_obj_t eccles_gui_panel_create(eccles_gui_t *gui, eccles_gui_obj_t parent) {
    if (!gui || !gui->ready) return ECCLES_GUI_NO_OBJ;
    eccles_gui_obj_t o = g_alloc(gui, ECCLES_GUI_W_PANEL, parent, ECCLES_GUI_STYLE_PANEL, 0);
    if (o != ECCLES_GUI_NO_OBJ) g_inv(gui, o);
    return o;
}
static eccles_gui_obj_t g_create_text(eccles_gui_t *gui, eccles_gui_obj_t parent, uint8_t type, uint8_t style, uint8_t flags, uint8_t mode, const char *text) {
    if (!gui || !gui->ready) return ECCLES_GUI_NO_OBJ;
    eccles_gui_obj_t o = g_alloc(gui, type, parent, style, flags);
    if (o == ECCLES_GUI_NO_OBJ) return o;
    gui->w[o].u.label.text = text ? text : "";
    gui->w[o].aux = (uint8_t)(mode | G_AUTO);
    g_autosize(gui, o);
    g_inv(gui, o);
    return o;
}
eccles_gui_obj_t eccles_gui_label_create(eccles_gui_t *gui, eccles_gui_obj_t parent, const char *text) {
    return g_create_text(gui, parent, ECCLES_GUI_W_LABEL, ECCLES_GUI_STYLE_LABEL, 0, ECCLES_GUI_TEXT_LEFT, text);
}
eccles_gui_obj_t eccles_gui_button_create(eccles_gui_t *gui, eccles_gui_obj_t parent, const char *text) {
    return g_create_text(gui, parent, ECCLES_GUI_W_BUTTON, ECCLES_GUI_STYLE_BUTTON, ECCLES_GUI_F_CLICKABLE, ECCLES_GUI_TEXT_CENTER, text);
}
eccles_gui_obj_t eccles_gui_checkbox_create(eccles_gui_t *gui, eccles_gui_obj_t parent, const char *text, bool checked) {
    eccles_gui_obj_t o = g_create_text(gui, parent, ECCLES_GUI_W_CHECKBOX, ECCLES_GUI_STYLE_CARD, ECCLES_GUI_F_CLICKABLE, ECCLES_GUI_TEXT_LEFT, text);
    if (o != ECCLES_GUI_NO_OBJ && checked) gui->w[o].flags |= ECCLES_GUI_F_CHECKED;
    return o;
}
eccles_gui_obj_t eccles_gui_image_create(eccles_gui_t *gui, eccles_gui_obj_t parent, const eccles_gui_image_t *img) {
    if (!gui || !gui->ready) return ECCLES_GUI_NO_OBJ;
    eccles_gui_obj_t o = g_alloc(gui, ECCLES_GUI_W_IMAGE, parent, ECCLES_GUI_STYLE_LABEL, 0);
    if (o == ECCLES_GUI_NO_OBJ) return o;
    gui->w[o].u.image.img = img;
    if (img) { gui->w[o].w = (int16_t)img->w; gui->w[o].h = (int16_t)img->h; }
    g_inv(gui, o);
    return o;
}
static eccles_gui_obj_t g_create_range(eccles_gui_t *gui, eccles_gui_obj_t parent, uint8_t type, int w, int h, int min, int max, int value) {
    if (!gui || !gui->ready) return ECCLES_GUI_NO_OBJ;
    eccles_gui_obj_t o = g_alloc(gui, type, parent, ECCLES_GUI_STYLE_CARD, type == ECCLES_GUI_W_SLIDER ? ECCLES_GUI_F_CLICKABLE : 0);
    if (o == ECCLES_GUI_NO_OBJ) return o;
    if (max < min) max = min;
    gui->w[o].w = (int16_t)w; gui->w[o].h = (int16_t)h;
    gui->w[o].u.range.min = (int16_t)min; gui->w[o].u.range.max = (int16_t)max;
    gui->w[o].u.range.val = (int16_t)G_CLAMP(value, min, max);
    g_inv(gui, o);
    return o;
}
eccles_gui_obj_t eccles_gui_slider_create(eccles_gui_t *gui, eccles_gui_obj_t parent, int min, int max, int value) {
    return g_create_range(gui, parent, ECCLES_GUI_W_SLIDER, 120, 20, min, max, value);
}
eccles_gui_obj_t eccles_gui_progress_create(eccles_gui_t *gui, eccles_gui_obj_t parent, int min, int max, int value) {
    return g_create_range(gui, parent, ECCLES_GUI_W_PROGRESS, 120, 12, min, max, value);
}
eccles_gui_obj_t eccles_gui_switch_create(eccles_gui_t *gui, eccles_gui_obj_t parent, bool on) {
    if (!gui || !gui->ready) return ECCLES_GUI_NO_OBJ;
    eccles_gui_obj_t o = g_alloc(gui, ECCLES_GUI_W_SWITCH, parent, ECCLES_GUI_STYLE_CARD, ECCLES_GUI_F_CLICKABLE);
    if (o == ECCLES_GUI_NO_OBJ) return o;
    gui->w[o].w = 48; gui->w[o].h = 24;
    if (on) gui->w[o].flags |= ECCLES_GUI_F_CHECKED;
    g_inv(gui, o);
    return o;
}
eccles_gui_obj_t eccles_gui_arc_create(eccles_gui_t *gui, eccles_gui_obj_t parent, int min, int max, int value) {
    eccles_gui_obj_t o = g_create_range(gui, parent, ECCLES_GUI_W_ARC, 80, 80, min, max, value);
    if (o == ECCLES_GUI_NO_OBJ) return o;
    //the range struct is shorter than the arc one, so move the value across
    int16_t mn = gui->w[o].u.range.min, mx = gui->w[o].u.range.max, v = gui->w[o].u.range.val;
    gui->w[o].u.arc.min = mn; gui->w[o].u.arc.max = mx; gui->w[o].u.arc.val = v;
    gui->w[o].u.arc.a0 = 135; gui->w[o].u.arc.a1 = 405;
    gui->w[o].aux = 8;  //thickness
    return o;
}
eccles_gui_obj_t eccles_gui_list_create(eccles_gui_t *gui, eccles_gui_obj_t parent, const char *const *items, int count) {
    if (!gui || !gui->ready) return ECCLES_GUI_NO_OBJ;
    eccles_gui_obj_t o = g_alloc(gui, ECCLES_GUI_W_LIST, parent, ECCLES_GUI_STYLE_CARD, ECCLES_GUI_F_CLICKABLE);
    if (o == ECCLES_GUI_NO_OBJ) return o;
    gui->w[o].w = 160; gui->w[o].h = 120;
    gui->w[o].u.list.items = items;
    gui->w[o].u.list.count = (uint16_t)G_CLAMP(count, 0, 65534);
    gui->w[o].u.list.sel = 0xFFFFu;
    g_inv(gui, o);
    return o;
}
eccles_gui_obj_t eccles_gui_custom_create(eccles_gui_t *gui, eccles_gui_obj_t parent, eccles_gui_draw_fn fn, void *user) {
    if (!gui || !gui->ready) return ECCLES_GUI_NO_OBJ;
    eccles_gui_obj_t o = g_alloc(gui, ECCLES_GUI_W_CUSTOM, parent, ECCLES_GUI_STYLE_PANEL, 0);
    if (o == ECCLES_GUI_NO_OBJ) return o;
    gui->w[o].u.custom.fn = fn;
    gui->w[o].user = user;
    g_inv(gui, o);
    return o;
}

//content setters

eccles_gui_status_t eccles_gui_set_text(eccles_gui_t *gui, eccles_gui_obj_t obj, const char *text) {
    if (!g_ok(gui, obj)) { g_err(gui, ECCLES_GUI_ERR_BAD_OBJECT, "obj"); return ECCLES_GUI_FAILED; }
    uint8_t t = gui->w[obj].type;
    if (t != ECCLES_GUI_W_LABEL && t != ECCLES_GUI_W_BUTTON && t != ECCLES_GUI_W_CHECKBOX) { g_err(gui, ECCLES_GUI_ERR_BAD_TYPE, "type"); return ECCLES_GUI_FAILED; }
    g_inv(gui, obj);
    gui->w[obj].u.label.text = text ? text : "";
    g_autosize(gui, obj);
    g_inv(gui, obj);
    return ECCLES_GUI_SUCCESSFUL;
}
eccles_gui_status_t eccles_gui_set_text_mode(eccles_gui_t *gui, eccles_gui_obj_t obj, uint8_t mode) {
    if (!g_ok(gui, obj)) { g_err(gui, ECCLES_GUI_ERR_BAD_OBJECT, "obj"); return ECCLES_GUI_FAILED; }
    uint8_t t = gui->w[obj].type;
    if (t != ECCLES_GUI_W_LABEL && t != ECCLES_GUI_W_BUTTON && t != ECCLES_GUI_W_CHECKBOX) { g_err(gui, ECCLES_GUI_ERR_BAD_TYPE, "type"); return ECCLES_GUI_FAILED; }
    g_inv(gui, obj);
    gui->w[obj].aux = (uint8_t)((gui->w[obj].aux & ~G_TEXT_MODE_MASK) | (mode & G_TEXT_MODE_MASK));
    g_autosize(gui, obj);
    g_inv(gui, obj);
    return ECCLES_GUI_SUCCESSFUL;
}
eccles_gui_status_t eccles_gui_image_set(eccles_gui_t *gui, eccles_gui_obj_t obj, const eccles_gui_image_t *img) {
    if (!g_ok_t(gui, obj, ECCLES_GUI_W_IMAGE)) return ECCLES_GUI_FAILED;
    g_inv(gui, obj);
    gui->w[obj].u.image.img = img;
    g_inv(gui, obj);
    return ECCLES_GUI_SUCCESSFUL;
}

static bool g_is_range(uint8_t t) { return t == ECCLES_GUI_W_SLIDER || t == ECCLES_GUI_W_PROGRESS || t == ECCLES_GUI_W_ARC; }
//slider/progress use the range struct, the arc has its own with the same first three fields in another place
static int16_t *g_val(eccles_gui_widget_t *w) { return w->type == ECCLES_GUI_W_ARC ? &w->u.arc.val : &w->u.range.val; }
static int16_t *g_min(eccles_gui_widget_t *w) { return w->type == ECCLES_GUI_W_ARC ? &w->u.arc.min : &w->u.range.min; }
static int16_t *g_max(eccles_gui_widget_t *w) { return w->type == ECCLES_GUI_W_ARC ? &w->u.arc.max : &w->u.range.max; }

eccles_gui_status_t eccles_gui_set_range(eccles_gui_t *gui, eccles_gui_obj_t obj, int min, int max) {
    if (!g_ok(gui, obj)) { g_err(gui, ECCLES_GUI_ERR_BAD_OBJECT, "obj"); return ECCLES_GUI_FAILED; }
    eccles_gui_widget_t *w = &gui->w[obj];
    if (!g_is_range(w->type)) { g_err(gui, ECCLES_GUI_ERR_BAD_TYPE, "type"); return ECCLES_GUI_FAILED; }
    if (max < min) max = min;
    *g_min(w) = (int16_t)min; *g_max(w) = (int16_t)max;
    *g_val(w) = (int16_t)G_CLAMP(*g_val(w), min, max);
    g_inv(gui, obj);
    return ECCLES_GUI_SUCCESSFUL;
}
eccles_gui_status_t eccles_gui_set_value(eccles_gui_t *gui, eccles_gui_obj_t obj, int value) {
    if (!g_ok(gui, obj)) { g_err(gui, ECCLES_GUI_ERR_BAD_OBJECT, "obj"); return ECCLES_GUI_FAILED; }
    eccles_gui_widget_t *w = &gui->w[obj];
    if (!g_is_range(w->type)) { g_err(gui, ECCLES_GUI_ERR_BAD_TYPE, "type"); return ECCLES_GUI_FAILED; }
    int16_t v = (int16_t)G_CLAMP(value, *g_min(w), *g_max(w));
    if (v != *g_val(w)) { *g_val(w) = v; g_inv(gui, obj); }
    return ECCLES_GUI_SUCCESSFUL;
}
int eccles_gui_get_value(const eccles_gui_t *gui, eccles_gui_obj_t obj) {
    if (!g_ok(gui, obj)) return 0;
    const eccles_gui_widget_t *w = &gui->w[obj];
    switch (w->type) {
    case ECCLES_GUI_W_SLIDER: case ECCLES_GUI_W_PROGRESS: return w->u.range.val;
    case ECCLES_GUI_W_ARC: return w->u.arc.val;
    case ECCLES_GUI_W_SWITCH: case ECCLES_GUI_W_CHECKBOX: return (w->flags & ECCLES_GUI_F_CHECKED) ? 1 : 0;
    case ECCLES_GUI_W_LIST: return w->u.list.sel == 0xFFFFu ? -1 : (int)w->u.list.sel;
    default: return 0;
    }
}
eccles_gui_status_t eccles_gui_set_checked(eccles_gui_t *gui, eccles_gui_obj_t obj, bool checked) {
    if (!g_ok(gui, obj)) { g_err(gui, ECCLES_GUI_ERR_BAD_OBJECT, "obj"); return ECCLES_GUI_FAILED; }
    if (gui->w[obj].type != ECCLES_GUI_W_SWITCH && gui->w[obj].type != ECCLES_GUI_W_CHECKBOX) { g_err(gui, ECCLES_GUI_ERR_BAD_TYPE, "type"); return ECCLES_GUI_FAILED; }
    g_set_flag(gui, obj, ECCLES_GUI_F_CHECKED, checked);
    return ECCLES_GUI_SUCCESSFUL;
}
bool eccles_gui_is_checked(const eccles_gui_t *gui, eccles_gui_obj_t obj) { return g_ok(gui, obj) && (gui->w[obj].flags & ECCLES_GUI_F_CHECKED); }

eccles_gui_status_t eccles_gui_arc_set_angles(eccles_gui_t *gui, eccles_gui_obj_t obj, int a0, int a1) {
    if (!g_ok_t(gui, obj, ECCLES_GUI_W_ARC)) return ECCLES_GUI_FAILED;
    gui->w[obj].u.arc.a0 = (int16_t)a0; gui->w[obj].u.arc.a1 = (int16_t)a1;
    g_inv(gui, obj);
    return ECCLES_GUI_SUCCESSFUL;
}
eccles_gui_status_t eccles_gui_arc_set_thickness(eccles_gui_t *gui, eccles_gui_obj_t obj, int thickness) {
    if (!g_ok_t(gui, obj, ECCLES_GUI_W_ARC)) return ECCLES_GUI_FAILED;
    gui->w[obj].aux = (uint8_t)G_CLAMP(thickness, 1, 255);
    g_inv(gui, obj);
    return ECCLES_GUI_SUCCESSFUL;
}

//rows of a list: font height plus padding
static int g_list_ih(const eccles_gui_t *gui, const eccles_gui_widget_t *w) {
    const eccles_gui_style_t *s = &gui->styles[w->style];
    return eccles_gui_font_height(gui, s->font) + 2 * G_MAX((int)s->pad, 2);
}
static int g_list_max_scroll(const eccles_gui_t *gui, const eccles_gui_widget_t *w) {
    int m = (int)w->u.list.count * g_list_ih(gui, w) - w->h;
    return m > 0 ? m : 0;
}
eccles_gui_status_t eccles_gui_list_set_items(eccles_gui_t *gui, eccles_gui_obj_t obj, const char *const *items, int count) {
    if (!g_ok_t(gui, obj, ECCLES_GUI_W_LIST)) return ECCLES_GUI_FAILED;
    eccles_gui_widget_t *w = &gui->w[obj];
    w->u.list.items = items;
    w->u.list.count = (uint16_t)G_CLAMP(count, 0, 65534);
    w->u.list.sel = 0xFFFFu;
    w->u.list.scroll = (int16_t)G_MIN((int)w->u.list.scroll, g_list_max_scroll(gui, w));
    g_inv(gui, obj);
    return ECCLES_GUI_SUCCESSFUL;
}
eccles_gui_status_t eccles_gui_list_set_selected(eccles_gui_t *gui, eccles_gui_obj_t obj, int index) {
    if (!g_ok_t(gui, obj, ECCLES_GUI_W_LIST)) return ECCLES_GUI_FAILED;
    eccles_gui_widget_t *w = &gui->w[obj];
    w->u.list.sel = (index < 0 || index >= (int)w->u.list.count) ? 0xFFFFu : (uint16_t)index;
    g_inv(gui, obj);
    return ECCLES_GUI_SUCCESSFUL;
}
eccles_gui_status_t eccles_gui_list_scroll_to(eccles_gui_t *gui, eccles_gui_obj_t obj, int pixels) {
    if (!g_ok_t(gui, obj, ECCLES_GUI_W_LIST)) return ECCLES_GUI_FAILED;
    eccles_gui_widget_t *w = &gui->w[obj];
    int v = G_CLAMP(pixels, 0, g_list_max_scroll(gui, w));
    if (v != w->u.list.scroll) { w->u.list.scroll = (int16_t)v; g_inv(gui, obj); }
    return ECCLES_GUI_SUCCESSFUL;
}
eccles_gui_status_t eccles_gui_custom_set_draw(eccles_gui_t *gui, eccles_gui_obj_t obj, eccles_gui_draw_fn fn, void *user) {
    if (!g_ok_t(gui, obj, ECCLES_GUI_W_CUSTOM)) return ECCLES_GUI_FAILED;
    gui->w[obj].u.custom.fn = fn;
    gui->w[obj].user = user;
    g_inv(gui, obj);
    return ECCLES_GUI_SUCCESSFUL;
}

//drawing the tree

//background, border and bevel of a widget from its style. `opa` is the final widget opacity
static void g_box(eccles_gui_dc_t *dc, const eccles_gui_style_t *s, int x, int y, int w, int h, uint32_t opa, bool pressed, bool force_solid) {
    if (opa == 0) return;
    if (s->shadow) eccles_gui_draw_shadow(dc, x, y, w, h, s->radius, s->shadow, (uint8_t)opa);
    uint32_t bo = force_solid ? 255u : g_opa_mul(s->opa, opa);
    uint16_t bg = s->bg, bg2 = s->bg2;
    if (pressed) { bg = eccles_gui_color_mix(bg, s->text, 40); bg2 = eccles_gui_color_mix(bg2, s->text, 40); }
    if (bo) {
        if (s->grad) eccles_gui_draw_gradient(dc, x, y, w, h, s->radius, bg, bg2, s->grad, (uint8_t)bo);
        else eccles_gui_draw_rrect(dc, x, y, w, h, s->radius, bg, (uint8_t)bo);
    }
    if (s->border_w) eccles_gui_draw_border(dc, x, y, w, h, s->radius, s->border_w, s->border, (uint8_t)opa);
    if (s->bevel) {
        int b = s->bevel < 0 ? -s->bevel : s->bevel, in = s->border_w, r = G_MIN((int)s->radius, G_MIN(w, h) / 2);
        bool raised = (s->bevel > 0) != pressed;
        uint16_t hi = eccles_gui_color_mix(bg, 0xFFFF, 110), lo = eccles_gui_color_mix(bg, 0x0000, 120);
        uint16_t tl = raised ? hi : lo, br = raised ? lo : hi;
        int ix = x + in, iy = y + in, iw = w - 2 * in, ih = h - 2 * in, ir = G_MAX(r - in, 0);
        eccles_gui_draw_fill(dc, ix + ir, iy, iw - 2 * ir, b, tl, (uint8_t)opa);
        eccles_gui_draw_fill(dc, ix, iy + ir, b, ih - 2 * ir, tl, (uint8_t)opa);
        eccles_gui_draw_fill(dc, ix + ir, iy + ih - b, iw - 2 * ir, b, br, (uint8_t)opa);
        eccles_gui_draw_fill(dc, ix + iw - b, iy + ir, b, ih - 2 * ir, br, (uint8_t)opa);
    }
}

static int32_t g_frac_q16(int val, int min, int max) {  //0..65536
    if (max <= min) return 0;
    return (int32_t)(((int64_t)(val - min) * 65536) / (max - min));
}

static void g_draw_content(eccles_gui_dc_t *dc, eccles_gui_t *gui, eccles_gui_obj_t idx, const eccles_gui_area_t *a, uint32_t opa) {
    eccles_gui_widget_t *w = &gui->w[idx];
    const eccles_gui_style_t *s = &gui->styles[w->style];
    int x = a->x1, y = a->y1, W = w->w, H = w->h;
    bool pressed = (w->flags & ECCLES_GUI_F_PRESSED) != 0;
    switch (w->type) {
    case ECCLES_GUI_W_LABEL: {
        eccles_gui_area_t box = g_area_make(x + s->pad, y + s->pad, W - 2 * s->pad, H - 2 * s->pad);
        eccles_gui_draw_text_box(dc, &box, s->font, w->u.label.text, s->text, (uint8_t)opa, (uint8_t)(w->aux & G_TEXT_MODE_MASK));
        break;
    }
    case ECCLES_GUI_W_BUTTON: {
        int iw = W - 2 * s->pad;
        bool wrap = (w->aux & ECCLES_GUI_TEXT_WRAP) != 0;
        int th = eccles_gui_text_height(gui, s->font, w->u.label.text, wrap ? iw : 0);
        eccles_gui_area_t box = g_area_make(x + s->pad, y + (H - th) / 2, iw, th > 0 ? th : 1);
        eccles_gui_draw_text_box(dc, &box, s->font, w->u.label.text, s->text, (uint8_t)opa, (uint8_t)(w->aux & G_TEXT_MODE_MASK));
        break;
    }
    case ECCLES_GUI_W_IMAGE: {
        const eccles_gui_image_t *img = w->u.image.img;
        if (img) eccles_gui_draw_image(dc, x + (W - img->w) / 2, y + (H - img->h) / 2, img, s->text, (uint8_t)opa);
        break;
    }
    case ECCLES_GUI_W_CHECKBOX: {
        int fh = eccles_gui_font_height(gui, s->font), bs = G_MIN(H, fh + 6), by = y + (H - bs) / 2;
        bool on = (w->flags & ECCLES_GUI_F_CHECKED) != 0;
        int r = G_MAX(2, bs / 5);
        if (on) eccles_gui_draw_rrect(dc, x, by, bs, bs, r, s->accent, (uint8_t)opa);
        else {
            eccles_gui_draw_rrect(dc, x, by, bs, bs, r, pressed ? s->bg2 : s->bg, (uint8_t)opa);
            eccles_gui_draw_border(dc, x, by, bs, bs, r, G_MAX(1, bs / 12), s->border, (uint8_t)opa);
        }
        if (on) {
            int lw = G_MAX(2, bs / 7);
            eccles_gui_draw_line(dc, x + bs * 22 / 100, by + bs * 54 / 100, x + bs * 43 / 100, by + bs * 74 / 100, lw, 0xFFFF, (uint8_t)opa);
            eccles_gui_draw_line(dc, x + bs * 43 / 100, by + bs * 74 / 100, x + bs * 80 / 100, by + bs * 28 / 100, lw, 0xFFFF, (uint8_t)opa);
        }
        eccles_gui_area_t box = g_area_make(x + bs + s->pad, y + (H - fh) / 2, W - bs - s->pad, fh);
        eccles_gui_draw_text_box(dc, &box, s->font, w->u.label.text, s->text, (uint8_t)opa, ECCLES_GUI_TEXT_CLIP);
        break;
    }
    case ECCLES_GUI_W_SWITCH: {
        bool on = (w->flags & ECCLES_GUI_F_CHECKED) != 0;
        int pad = G_MAX(2, H / 8), d = H - 2 * pad;
        eccles_gui_draw_rrect(dc, x, y, W, H, H / 2, on ? s->accent : s->bg2, (uint8_t)opa);
        int kx = on ? x + W - pad - d : x + pad;
        eccles_gui_draw_rrect(dc, kx, y + pad, d, d, d / 2, 0xFFFF, (uint8_t)opa);
        break;
    }
    case ECCLES_GUI_W_SLIDER: {
        bool horiz = W >= H;
        int len = horiz ? W : H, thick = horiz ? H : W;
        int kr = thick / 2, th = G_MAX(4, thick / 4);
        int32_t f = g_frac_q16(w->u.range.val, w->u.range.min, w->u.range.max);
        int span = len - 2 * kr;
        int kc = kr + (int)(((int64_t)span * f) >> 16);  //knob center along the slider
        int tpos = (thick - th) / 2;
        if (horiz) {
            eccles_gui_draw_rrect(dc, x, y + tpos, W, th, th / 2, s->bg2, (uint8_t)opa);
            if (kc > 0) eccles_gui_draw_rrect(dc, x, y + tpos, kc, th, th / 2, s->accent, (uint8_t)opa);
            eccles_gui_draw_disc(dc, x + kc, y + H / 2, kr, s->accent, (uint8_t)opa);
            eccles_gui_draw_disc(dc, x + kc, y + H / 2, G_MAX(1, kr - G_MAX(2, kr / 3)), s->text, (uint8_t)opa);
        } else {
            int ky = y + H - kc;
            eccles_gui_draw_rrect(dc, x + tpos, y, th, H, th / 2, s->bg2, (uint8_t)opa);
            if (kc > 0) eccles_gui_draw_rrect(dc, x + tpos, ky, th, y + H - ky, th / 2, s->accent, (uint8_t)opa);
            eccles_gui_draw_disc(dc, x + W / 2, ky, kr, s->accent, (uint8_t)opa);
            eccles_gui_draw_disc(dc, x + W / 2, ky, G_MAX(1, kr - G_MAX(2, kr / 3)), s->text, (uint8_t)opa);
        }
        break;
    }
    case ECCLES_GUI_W_PROGRESS: {
        bool horiz = W >= H;
        int r = G_MIN(W, H) / 2;
        int32_t f = g_frac_q16(w->u.range.val, w->u.range.min, w->u.range.max);
        eccles_gui_draw_rrect(dc, x, y, W, H, r, s->bg2, (uint8_t)opa);
        if (horiz) {
            int fl = (int)(((int64_t)W * f) >> 16);
            if (fl > 0) eccles_gui_draw_rrect(dc, x, y, fl, H, r, s->accent, (uint8_t)opa);
        } else {
            int fl = (int)(((int64_t)H * f) >> 16);
            if (fl > 0) eccles_gui_draw_rrect(dc, x, y + H - fl, W, fl, r, s->accent, (uint8_t)opa);
        }
        break;
    }
    case ECCLES_GUI_W_ARC: {
        int r = G_MIN(W, H) / 2, cx = x + W / 2, cy = y + H / 2, th = w->aux ? w->aux : 8;
        int a0 = w->u.arc.a0, a1 = w->u.arc.a1;
        eccles_gui_draw_arc(dc, cx, cy, r, th, a0, a1, true, s->bg2, (uint8_t)opa);
        int32_t f = g_frac_q16(w->u.arc.val, w->u.arc.min, w->u.arc.max);
        int sweep = (int)(((int64_t)(a1 - a0) * f) >> 16);
        if (sweep > 0) eccles_gui_draw_arc(dc, cx, cy, r, th, a0, a0 + sweep, true, s->accent, (uint8_t)opa);
        break;
    }
    case ECCLES_GUI_W_LIST: {
        int ih = g_list_ih(gui, w), fh = eccles_gui_font_height(gui, s->font), pad = G_MAX((int)s->pad, 2);
        int first = w->u.list.scroll / ih;
        for (int i = first; i < w->u.list.count; i++) {
            int ry = y + i * ih - w->u.list.scroll;
            if (ry >= a->y2) break;
            if (ry + ih <= dc->clip.y1) continue;
            const char *t = w->u.list.items ? w->u.list.items[i] : "";
            if (i == w->u.list.sel) eccles_gui_draw_fill(dc, x, ry, W, ih, s->accent, (uint8_t)g_opa_mul(opa, 90u));
            if (i + 1 < w->u.list.count) eccles_gui_draw_fill(dc, x + pad, ry + ih - 1, W - 2 * pad, 1, s->bg2, (uint8_t)opa);
            eccles_gui_area_t box = g_area_make(x + pad * 2, ry + (ih - fh) / 2, W - pad * 4, fh);
            eccles_gui_draw_text_box(dc, &box, s->font, t ? t : "", s->text, (uint8_t)opa, ECCLES_GUI_TEXT_ELLIPSIS);
        }
        break;
    }
    case ECCLES_GUI_W_CUSTOM:
        if (w->u.custom.fn) w->u.custom.fn(dc, a, w->user);
        break;
#if ECCLES_GUI_HAS_3D && ECCLES_GUI_MAX_VIEWS3D > 0
    case ECCLES_GUI_W_VIEW3D:
        if (w->u.v3d.view < ECCLES_GUI_MAX_VIEWS3D) eccles_gui_draw_view3d(dc, &gui->views[w->u.v3d.view], x, y, W, H);
        break;
#endif
    default: break;
    }
}

static void g_draw_widget(eccles_gui_dc_t *dc, eccles_gui_t *gui, eccles_gui_obj_t idx, int px, int py, uint32_t popa) {
    eccles_gui_widget_t *w = &gui->w[idx];
    if (w->flags & ECCLES_GUI_F_HIDDEN) return;
    int rx, ry;
    g_rel(gui, w, &rx, &ry);
    int ax = px + rx, ay = py + ry;
    eccles_gui_area_t area = g_area_make(ax, ay, w->w, w->h);
    uint32_t opa = g_opa_mul(popa, w->opa);
    if (w->flags & ECCLES_GUI_F_DISABLED) opa = opa * 100u / 255u;
    const eccles_gui_style_t *s = &gui->styles[w->style];
    int sh = s->shadow ? s->shadow + s->shadow / 3 + 2 : 0;
    eccles_gui_area_t saved = dc->clip;
    eccles_gui_area_t outer = area;
    outer.x1 = (int16_t)(outer.x1 - sh); outer.y1 = (int16_t)(outer.y1 - sh); outer.x2 = (int16_t)(outer.x2 + sh); outer.y2 = (int16_t)(outer.y2 + sh);
    dc->clip = g_area_isect(&saved, &outer);
    if (g_area_empty(&dc->clip)) { dc->clip = saved; return; }
    bool pressed = (w->flags & ECCLES_GUI_F_PRESSED) != 0;
    uint8_t t = w->type;
    //own box: panels, cards, buttons, lists and images. the root is always solid
    if (idx == 0) {  //the root must paint every pixel of the panel: no rounded corners, no shadow
        eccles_gui_style_t rs = *s;
        rs.radius = 0; rs.shadow = 0;
        g_box(dc, &rs, ax, ay, w->w, w->h, 255, false, true);
    }
    else if (t == ECCLES_GUI_W_PANEL || t == ECCLES_GUI_W_BUTTON || t == ECCLES_GUI_W_LIST || t == ECCLES_GUI_W_IMAGE || t == ECCLES_GUI_W_LABEL)
        g_box(dc, s, ax, ay, w->w, w->h, opa, pressed && t == ECCLES_GUI_W_BUTTON, false);
    dc->clip = g_area_isect(&saved, &area);
    if (!g_area_empty(&dc->clip)) {
        g_draw_content(dc, gui, idx, &area, opa);
        for (eccles_gui_obj_t c = w->child; c != ECCLES_GUI_NO_OBJ; c = gui->w[c].next) g_draw_widget(dc, gui, c, ax, ay, opa);
    }
    dc->clip = saved;
}

static void g_draw_tree(eccles_gui_dc_t *dc, const eccles_gui_area_t *area, void *user) {
    (void)area; (void)user;
    g_draw_widget(dc, dc->gui, 0, 0, 0, 255);
}

#endif  //ECCLES_GUI_HAS_WIDGETS

//7. input

#if ECCLES_GUI_HAS_WIDGETS

static void g_emit(eccles_gui_t *gui, eccles_gui_obj_t obj, eccles_gui_ev_t code, int x, int y, int dx, int dy, int32_t value) {
    if (!g_ok(gui, obj) || !gui->w[obj].cb) return;
    eccles_gui_event_t ev;
    ev.code = code; ev.obj = obj;
    ev.x = (int16_t)x; ev.y = (int16_t)y; ev.dx = (int16_t)dx; ev.dy = (int16_t)dy;
    ev.value = value; ev.user = gui->w[obj].user;
    gui->w[obj].cb(gui, &ev);
}

//gestures go up the tree to the first widget that listens
static void g_emit_bubble(eccles_gui_t *gui, eccles_gui_obj_t obj, eccles_gui_ev_t code, int x, int y, int dx, int dy, int32_t value) {
    for (int guard = 0; guard < 64 && g_ok(gui, obj); guard++) {
        if (gui->w[obj].cb) { g_emit(gui, obj, code, x, y, dx, dy, value); return; }
        obj = gui->w[obj].parent;
    }
}

static void g_slider_touch(eccles_gui_t *gui, eccles_gui_obj_t obj, int x, int y) {
    eccles_gui_widget_t *w = &gui->w[obj];
    int ax, ay;
    g_abs(gui, obj, &ax, &ay);
    bool horiz = w->w >= w->h;
    int len = horiz ? w->w : w->h, kr = (horiz ? w->h : w->w) / 2, span = len - 2 * kr;
    int pos = horiz ? x - ax - kr : (ay + w->h - kr) - y;
    pos = G_CLAMP(pos, 0, span > 0 ? span : 0);
    int range = w->u.range.max - w->u.range.min;
    int v = w->u.range.min + (span > 0 ? (int)(((int64_t)pos * range + span / 2) / span) : 0);
    if (v != w->u.range.val) {
        w->u.range.val = (int16_t)v;
        g_inv(gui, obj);
        g_emit(gui, obj, ECCLES_GUI_EV_VALUE_CHANGED, x, y, 0, 0, v);
    }
}

static void g_list_scroll_by(eccles_gui_t *gui, eccles_gui_obj_t obj, int d) {
    eccles_gui_widget_t *w = &gui->w[obj];
    int v = G_CLAMP((int)w->u.list.scroll + d, 0, g_list_max_scroll(gui, w));
    if (v != w->u.list.scroll) { w->u.list.scroll = (int16_t)v; g_inv(gui, obj); }
}

void eccles_gui_touch(eccles_gui_t *gui, int x, int y, bool pressed) {
    if (!gui || !gui->ready) return;
    if (pressed) {
        if (!gui->t_down) {
            gui->t_down = 1; gui->t_moved = 0; gui->t_long = 0;
            gui->t_x0 = gui->t_x = (int16_t)x; gui->t_y0 = gui->t_y = (int16_t)y;
            gui->t_ms = gui->t_vt = gui->now_ms;
            gui->t_vy = 0; gui->t_vacc = 0;
            gui->fling_obj = ECCLES_GUI_NO_OBJ;  //a touch stops a fling
            gui->t_obj = g_hit(gui, 0, 0, 0, x, y, true);
            eccles_gui_obj_t o = gui->t_obj;
            if (o != ECCLES_GUI_NO_OBJ) {
                gui->w[o].flags |= ECCLES_GUI_F_PRESSED;
                g_inv(gui, o);
                g_emit(gui, o, ECCLES_GUI_EV_PRESSED, x, y, 0, 0, 0);
                if (g_ok(gui, o) && gui->w[o].type == ECCLES_GUI_W_SLIDER) g_slider_touch(gui, o, x, y);
            }
        } else {
            int dx = x - gui->t_x, dy = y - gui->t_y;
            if (!dx && !dy) return;
            bool was_moved = gui->t_moved != 0;
            if (!was_moved && (G_ABS(x - gui->t_x0) > ECCLES_GUI_TOUCH_SLOP || G_ABS(y - gui->t_y0) > ECCLES_GUI_TOUCH_SLOP)) gui->t_moved = 1;
            gui->t_x = (int16_t)x; gui->t_y = (int16_t)y;
            eccles_gui_obj_t o = gui->t_obj;
            if (g_ok(gui, o)) {
                uint8_t type = gui->w[o].type;
                if (type == ECCLES_GUI_W_SLIDER) g_slider_touch(gui, o, x, y);
                else if (type == ECCLES_GUI_W_LIST && gui->t_moved) {
                    int d = was_moved ? dy : y - gui->t_y0;  //the first move also takes the distance spent inside the slop
                    g_list_scroll_by(gui, o, -d);
                    gui->t_vacc = (int16_t)(gui->t_vacc + d);
                    uint32_t dt = gui->now_ms - gui->t_vt;
                    if (dt >= 8) {
                        int v = (int)((int32_t)gui->t_vacc * 16 / (int32_t)dt);
                        gui->t_vy = (int16_t)(gui->t_vy == 0 ? v : (gui->t_vy + v) / 2);
                        gui->t_vacc = 0; gui->t_vt = gui->now_ms;
                    }
                }
                if (gui->t_moved) g_emit(gui, o, ECCLES_GUI_EV_DRAG, x, y, dx, dy, 0);
            }
        }
        return;
    }
    if (!gui->t_down) return;
    //release
    gui->t_down = 0;
    eccles_gui_obj_t o = gui->t_obj;
    gui->t_obj = ECCLES_GUI_NO_OBJ;
    if (!g_ok(gui, o)) return;
    gui->w[o].flags = (uint8_t)(gui->w[o].flags & ~ECCLES_GUI_F_PRESSED);
    g_inv(gui, o);
    g_emit(gui, o, ECCLES_GUI_EV_RELEASED, x, y, 0, 0, 0);
    if (!g_ok(gui, o)) return;
    uint8_t type = gui->w[o].type;
    eccles_gui_area_t a = g_obj_area(gui, o);
    bool inside = x >= a.x1 && x < a.x2 && y >= a.y1 && y < a.y2;
    if (!gui->t_moved && !gui->t_long && inside) {
        if (type == ECCLES_GUI_W_SWITCH || type == ECCLES_GUI_W_CHECKBOX) {
            gui->w[o].flags ^= ECCLES_GUI_F_CHECKED;
            g_inv(gui, o);
            g_emit(gui, o, ECCLES_GUI_EV_VALUE_CHANGED, x, y, 0, 0, (gui->w[o].flags & ECCLES_GUI_F_CHECKED) ? 1 : 0);
        } else if (type == ECCLES_GUI_W_LIST) {
            eccles_gui_widget_t *w = &gui->w[o];
            int idx = (y - a.y1 + w->u.list.scroll) / g_list_ih(gui, w);
            if (idx >= 0 && idx < (int)w->u.list.count) {
                if (w->u.list.sel != (uint16_t)idx) {
                    w->u.list.sel = (uint16_t)idx;
                    g_inv(gui, o);
                    g_emit(gui, o, ECCLES_GUI_EV_VALUE_CHANGED, x, y, 0, 0, idx);
                }
                g_emit(gui, o, ECCLES_GUI_EV_CLICKED, x, y, 0, 0, idx);
            }
            return;
        }
        g_emit(gui, o, ECCLES_GUI_EV_CLICKED, x, y, 0, 0, 0);
        return;
    }
    if (gui->t_moved && g_ok(gui, o)) {
        int dx = x - gui->t_x0, dy = y - gui->t_y0;
        if (type == ECCLES_GUI_W_LIST) {
            if (gui->now_ms - gui->t_vt <= 100 && G_ABS(gui->t_vy) >= 2) {  //still moving when the finger left: fling
                gui->fling_obj = o; gui->fling_v = (int16_t)(-gui->t_vy); gui->fling_acc = 0;
            }
        }
        if (type != ECCLES_GUI_W_SLIDER) {
            int dir = 0;
            if (G_ABS(dx) >= ECCLES_GUI_SWIPE_MIN && G_ABS(dx) > 2 * G_ABS(dy)) dir = dx < 0 ? ECCLES_GUI_DIR_LEFT : ECCLES_GUI_DIR_RIGHT;
            else if (type != ECCLES_GUI_W_LIST && G_ABS(dy) >= ECCLES_GUI_SWIPE_MIN && G_ABS(dy) > 2 * G_ABS(dx)) dir = dy < 0 ? ECCLES_GUI_DIR_UP : ECCLES_GUI_DIR_DOWN;
            if (dir) g_emit_bubble(gui, o, ECCLES_GUI_EV_GESTURE, x, y, dx, dy, dir);
        }
    }
}

static void g_input_tick(eccles_gui_t *gui, uint32_t dt) {
    if (gui->t_down && !gui->t_moved && !gui->t_long && g_ok(gui, gui->t_obj) && gui->now_ms - gui->t_ms >= ECCLES_GUI_LONG_PRESS_MS) {
        gui->t_long = 1;
        eccles_gui_obj_t o = gui->t_obj;
        g_emit(gui, o, ECCLES_GUI_EV_LONG_PRESSED, gui->t_x, gui->t_y, 0, 0, 0);
    }
    if (gui->fling_obj != ECCLES_GUI_NO_OBJ) {
        if (!g_ok(gui, gui->fling_obj) || gui->w[gui->fling_obj].type != ECCLES_GUI_W_LIST) { gui->fling_obj = ECCLES_GUI_NO_OBJ; return; }
        gui->fling_acc += dt;
        while (gui->fling_acc >= 16 && gui->fling_v != 0) {
            gui->fling_acc -= 16;
            eccles_gui_widget_t *w = &gui->w[gui->fling_obj];
            int before = w->u.list.scroll;
            g_list_scroll_by(gui, gui->fling_obj, gui->fling_v);
            if (w->u.list.scroll == before) gui->fling_v = 0;  //hit an end
            else gui->fling_v = (int16_t)((gui->fling_v * 15) / 16);
        }
        if (gui->fling_v == 0) gui->fling_obj = ECCLES_GUI_NO_OBJ;
    }
}

#endif  //ECCLES_GUI_HAS_WIDGETS

//8. animation

#if ECCLES_GUI_HAS_ANIM && ECCLES_GUI_MAX_ANIMS > 0

int32_t eccles_gui_ease(uint8_t e, int32_t t) {  //all in Q10
    t = G_CLAMP(t, 0, 1024);
    switch (e) {
    case ECCLES_GUI_EASE_IN: return (t * t) >> 10;
    case ECCLES_GUI_EASE_OUT: return 1024 - (((1024 - t) * (1024 - t)) >> 10);
    case ECCLES_GUI_EASE_IN_OUT: return t < 512 ? (2 * t * t) >> 10 : 1024 - ((2 * (1024 - t) * (1024 - t)) >> 10);
    case ECCLES_GUI_EASE_OUT_CUBIC: { int32_t u = 1024 - t; return 1024 - (((u * u >> 10) * u) >> 10); }
    case ECCLES_GUI_EASE_OUT_BACK: {
        int32_t u = t - 1024, u2 = (u * u) >> 10, u3 = (u2 * u) >> 10;
        return 1024 + ((2766 * u3) >> 10) + ((1742 * u2) >> 10);
    }
    case ECCLES_GUI_EASE_OUT_BOUNCE: {
        int32_t x = t;
        if (x < 372) return (7744 * ((x * x) >> 10)) >> 10;
        if (x < 744) { x -= 558; return ((7744 * ((x * x) >> 10)) >> 10) + 768; }
        if (x < 930) { x -= 837; return ((7744 * ((x * x) >> 10)) >> 10) + 960; }
        x -= 976;
        return ((7744 * ((x * x) >> 10)) >> 10) + 1008;
    }
    default: return t;
    }
}

int eccles_gui_anim_count(const eccles_gui_t *gui) {
    int n = 0;
    if (!gui) return 0;
    for (int i = 0; i < ECCLES_GUI_MAX_ANIMS; i++) if (gui->anims[i].used) n++;
    return n;
}

static bool g_anim_same(const eccles_gui_anim_t *a, const eccles_gui_anim_t *b) {
    if (a->prop != b->prop) return false;
    if (a->prop == ECCLES_GUI_ANIM_CUSTOM) return a->exec == b->exec && a->target == b->target;
    return a->obj == b->obj && a->index == b->index;
}

eccles_gui_status_t eccles_gui_anim_start(eccles_gui_t *gui, const eccles_gui_anim_t *anim) {
    if (!gui || !gui->ready || !anim || anim->prop < ECCLES_GUI_ANIM_X || anim->prop > ECCLES_GUI_ANIM_CUSTOM || anim->ease > ECCLES_GUI_EASE_OUT_BOUNCE) {
        g_err(gui, ECCLES_GUI_ERR_BAD_ARG, "anim"); return ECCLES_GUI_FAILED;
    }
    if (anim->prop == ECCLES_GUI_ANIM_CUSTOM) {
        if (!anim->exec) { g_err(gui, ECCLES_GUI_ERR_BAD_ARG, "exec"); return ECCLES_GUI_FAILED; }
    } else {
#if ECCLES_GUI_HAS_WIDGETS
        if (!g_ok(gui, anim->obj)) { g_err(gui, ECCLES_GUI_ERR_BAD_OBJECT, "obj"); return ECCLES_GUI_FAILED; }
#else
        g_err(gui, ECCLES_GUI_ERR_DISABLED, "widgets"); return ECCLES_GUI_UNSUPPORTED;
#endif
    }
    int slot = -1;
    for (int i = 0; i < ECCLES_GUI_MAX_ANIMS; i++) {
        if (gui->anims[i].used && g_anim_same(&gui->anims[i].a, anim)) { slot = i; break; }
        if (!gui->anims[i].used && slot < 0) slot = i;
    }
    if (slot < 0) { g_err(gui, ECCLES_GUI_ERR_NO_MEMORY, "anims"); return ECCLES_GUI_FAILED; }
    gui->anims[slot].a = *anim;
    gui->anims[slot].elapsed = 0;
    gui->anims[slot].used = 1;
    return ECCLES_GUI_SUCCESSFUL;
}

int eccles_gui_anim_cancel(eccles_gui_t *gui, eccles_gui_obj_t obj, uint8_t prop) {
    int n = 0;
    if (!gui) return 0;
    for (int i = 0; i < ECCLES_GUI_MAX_ANIMS; i++) {
        eccles_gui_anim_slot_t *s = &gui->anims[i];
        if (s->used && s->a.prop != ECCLES_GUI_ANIM_CUSTOM && s->a.obj == obj && (prop == 0 || s->a.prop == prop)) { s->used = 0; n++; }
    }
    return n;
}
int eccles_gui_anim_cancel_target(eccles_gui_t *gui, void *target) {
    int n = 0;
    if (!gui) return 0;
    for (int i = 0; i < ECCLES_GUI_MAX_ANIMS; i++) {
        eccles_gui_anim_slot_t *s = &gui->anims[i];
        if (s->used && s->a.prop == ECCLES_GUI_ANIM_CUSTOM && s->a.target == target) { s->used = 0; n++; }
    }
    return n;
}

static void g_anim_apply(eccles_gui_t *gui, const eccles_gui_anim_t *a, int32_t v) {
    if (a->prop == ECCLES_GUI_ANIM_CUSTOM) { a->exec(gui, a->target, v); return; }
#if ECCLES_GUI_HAS_WIDGETS
    if (!g_ok(gui, a->obj)) return;
    eccles_gui_widget_t *w = &gui->w[a->obj];
    switch (a->prop) {
    case ECCLES_GUI_ANIM_X: eccles_gui_obj_set_pos(gui, a->obj, (int)v, w->y); break;
    case ECCLES_GUI_ANIM_Y: eccles_gui_obj_set_pos(gui, a->obj, w->x, (int)v); break;
    case ECCLES_GUI_ANIM_W: eccles_gui_obj_set_size(gui, a->obj, (int)v, w->h); break;
    case ECCLES_GUI_ANIM_H: eccles_gui_obj_set_size(gui, a->obj, w->w, (int)v); break;
    case ECCLES_GUI_ANIM_OPA: eccles_gui_obj_set_opa(gui, a->obj, (uint8_t)G_CLAMP(v, 0, 255)); break;
    case ECCLES_GUI_ANIM_VALUE: if (g_is_range(w->type)) eccles_gui_set_value(gui, a->obj, (int)v); break;
#if ECCLES_GUI_HAS_3D && ECCLES_GUI_MAX_VIEWS3D > 0
    case ECCLES_GUI_ANIM_V3D_YAW: case ECCLES_GUI_ANIM_V3D_PITCH: case ECCLES_GUI_ANIM_V3D_ROLL: case ECCLES_GUI_ANIM_V3D_SCALE:
        if (w->type == ECCLES_GUI_W_VIEW3D && w->u.v3d.view < ECCLES_GUI_MAX_VIEWS3D && a->index < ECCLES_GUI_3D_MAX_OBJECTS) {
            eccles_gui_obj3d_t *o = &gui->views[w->u.v3d.view].obj[a->index];
            if (a->prop == ECCLES_GUI_ANIM_V3D_YAW) o->yaw = (uint16_t)v;
            else if (a->prop == ECCLES_GUI_ANIM_V3D_PITCH) o->pitch = (uint16_t)v;
            else if (a->prop == ECCLES_GUI_ANIM_V3D_ROLL) o->roll = (uint16_t)v;
            else o->scale = v;
            g_inv(gui, a->obj);
        }
        break;
#endif
    default: break;
    }
#endif
}

static void g_anim_step(eccles_gui_t *gui, uint32_t dt) {
    for (int i = 0; i < ECCLES_GUI_MAX_ANIMS; i++) {
        eccles_gui_anim_slot_t *s = &gui->anims[i];
        if (!s->used) continue;
        s->elapsed += dt;
        if (s->elapsed < s->a.delay_ms) continue;
        eccles_gui_anim_t a = s->a;  //callbacks may start or cancel animations, work on a copy
        uint32_t te = s->elapsed - a.delay_ms, dur = a.duration_ms ? a.duration_ms : 1u;
        uint32_t cycle = te / dur, frac = te % dur;
        bool finished = a.repeat != ECCLES_GUI_ANIM_REPEAT_FOREVER && cycle > a.repeat;
        if (finished) { cycle = a.repeat; frac = dur; }
        bool forward = !a.yoyo || (cycle & 1u) == 0;
        int32_t t = (int32_t)((frac * 1024u) / dur);
        if (!forward) t = 1024 - t;
        int32_t v = a.from + (int32_t)(((int64_t)(a.to - a.from) * eccles_gui_ease(a.ease, t)) / 1024);
        if (finished) s->used = 0;
        g_anim_apply(gui, &a, v);
        if (finished && a.done) a.done(gui, a.target);
    }
}

#endif  //ECCLES_GUI_HAS_ANIM

uint32_t eccles_gui_update(eccles_gui_t *gui, uint32_t now_ms) {
    if (!gui || !gui->ready) return 0;
    uint32_t dt = gui->have_time ? now_ms - gui->now_ms : 0;
    gui->now_ms = now_ms;
    gui->have_time = 1;
#if ECCLES_GUI_HAS_ANIM && ECCLES_GUI_MAX_ANIMS > 0
    g_anim_step(gui, dt);
#endif
#if ECCLES_GUI_HAS_WIDGETS
    g_input_tick(gui, dt);
#else
    (void)dt;
#endif
    return eccles_gui_render(gui);
}

//9. 3d
//pipeline: once per frame (cached for all bands) every object's vertices go through one 3x3 matrix, then a perspective
//divide, faces are culled (back faces by screen space winding, near plane, off screen) and lit, then sorted far to near
//(skipped when you give a z buffer). per band: every face that touches the band is rasterised into it

#if ECCLES_GUI_HAS_3D

typedef int32_t g_m3[9];  //row major, Q14

static void g_m3_mul(const g_m3 a, const g_m3 b, g_m3 r) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r[i * 3 + j] = (a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j]) >> 14;
}
static inline int32_t g_q14(int16_t q15) { return ((int32_t)q15 + 1) >> 1; }
static void g_m3_rx(g_m3 m, uint16_t a) {
    int32_t c = g_q14(eccles_gui_cos(a)), s = g_q14(eccles_gui_sin(a));
    m[0] = 16384; m[1] = 0; m[2] = 0; m[3] = 0; m[4] = c; m[5] = -s; m[6] = 0; m[7] = s; m[8] = c;
}
static void g_m3_ry(g_m3 m, uint16_t a) {
    int32_t c = g_q14(eccles_gui_cos(a)), s = g_q14(eccles_gui_sin(a));
    m[0] = c; m[1] = 0; m[2] = s; m[3] = 0; m[4] = 16384; m[5] = 0; m[6] = -s; m[7] = 0; m[8] = c;
}
static void g_m3_rz(g_m3 m, uint16_t a) {
    int32_t c = g_q14(eccles_gui_cos(a)), s = g_q14(eccles_gui_sin(a));
    m[0] = c; m[1] = -s; m[2] = 0; m[3] = s; m[4] = c; m[5] = 0; m[6] = 0; m[7] = 0; m[8] = 16384;
}

static inline uint8_t g_shade(int32_t d, int amb) {  //d = dot(normal, light) in Q14
    d = G_CLAMP(d, 0, 16384);
    return (uint8_t)(amb + (((255 - amb) * d) >> 14));
}

void eccles_gui_view3d_defaults(eccles_gui_view3d_t *sc, int view_h) {
    if (!sc) return;
    memset(sc, 0, sizeof *sc);
    sc->cam_z = -4 * ECCLES_GUI_ONE;
    sc->focal = (int16_t)G_CLAMP(view_h, 8, 30000);
    sc->near_z = ECCLES_GUI_ONE / 10;
    sc->light[0] = -50; sc->light[1] = 70; sc->light[2] = -90;
    sc->ambient = 56;
    for (int i = 0; i < ECCLES_GUI_3D_MAX_OBJECTS; i++) {
        sc->obj[i].scale = ECCLES_GUI_ONE;
        sc->obj[i].color = 0xFFFF;
        sc->obj[i].mode = ECCLES_GUI_3D_FLAT;
        sc->obj[i].flags = ECCLES_GUI_3D_F_VISIBLE;
    }
}

//unit normal (Q7) of a face computed from its vertices, for meshes that do not ship normals
static void g_face_normal(const eccles_gui_mesh_t *m, uint16_t ia, uint16_t ib, uint16_t ic, int32_t n[3]) {
    const int16_t *a = &m->verts[ia * 3], *b = &m->verts[ib * 3], *c = &m->verts[ic * 3];
    int32_t e1[3], e2[3], mx = 1;
    for (int i = 0; i < 3; i++) {
        e1[i] = b[i] - a[i]; e2[i] = c[i] - a[i];
        mx = G_MAX(mx, G_MAX(G_ABS(e1[i]), G_ABS(e2[i])));
    }
    int sh = 0;
    while ((mx >> sh) > 8000) sh++;
    for (int i = 0; i < 3; i++) { e1[i] >>= sh; e2[i] >>= sh; }
    int32_t cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2], cz = e1[0] * e2[1] - e1[1] * e2[0];
    int32_t big = G_MAX(G_ABS(cx), G_MAX(G_ABS(cy), G_ABS(cz)));
    int s2 = 0;
    while ((big >> s2) > 30000) s2++;
    cx >>= s2; cy >>= s2; cz >>= s2;
    int32_t len = (int32_t)g_isqrt((uint32_t)(cx * cx + cy * cy + cz * cz));
    if (len == 0) { n[0] = n[1] = n[2] = 0; return; }
    n[0] = cx * 127 / len; n[1] = cy * 127 / len; n[2] = cz * 127 / len;
}

static void g_prepare3d(eccles_gui_t *gui, const eccles_gui_view3d_t *sc, int vx, int vy, int vw, int vh) {
    gui->r3.scene = sc; gui->r3.frame = gui->frame;
    gui->r3.vx = (int16_t)vx; gui->r3.vy = (int16_t)vy; gui->r3.vw = (int16_t)vw; gui->r3.vh = (int16_t)vh;
    gui->r3.nv = gui->r3.nf = 0; gui->r3.dropped = 0;
    int cx = vx + vw / 2, cy = vy + vh / 2;
    int32_t lx = sc->light[0], ly = sc->light[1], lz = sc->light[2];
    int32_t ll = (int32_t)g_isqrt((uint32_t)(lx * lx + ly * ly + lz * lz));
    int32_t lw[3];
    if (ll == 0) { lw[0] = 0; lw[1] = 0; lw[2] = -16384; }
    else { lw[0] = lx * 16384 / ll; lw[1] = ly * 16384 / ll; lw[2] = lz * 16384 / ll; }
    g_m3 mcam, ra, rb;
    g_m3_ry(ra, (uint16_t)(0u - sc->cam_yaw));
    g_m3_rx(rb, (uint16_t)(0u - sc->cam_pitch));
    g_m3_mul(rb, ra, mcam);  //world -> view
    int amb = sc->ambient;
    bool no_cull = (sc->flags & ECCLES_GUI_3D_VIEW_NO_CULL) != 0;
    int32_t near_z = sc->near_z > 0 ? sc->near_z : 1;
    int focal = sc->focal > 0 ? sc->focal : 1;
    for (int oi = 0; oi < ECCLES_GUI_3D_MAX_OBJECTS; oi++) {
        const eccles_gui_obj3d_t *o = &sc->obj[oi];
        gui->r3.vbase[oi] = 0xFFFFu;
        const eccles_gui_mesh_t *m = o->mesh;
        if (!m || !(o->flags & ECCLES_GUI_3D_F_VISIBLE) || !m->verts || !m->faces || m->nverts == 0) continue;
        if (gui->r3.nv + m->nverts > ECCLES_GUI_3D_MAX_VERTS) { gui->r3.dropped = (uint16_t)(gui->r3.dropped + m->nfaces); continue; }
        g_m3 mm, t1, t2, mv, ms;
        g_m3_rz(t1, o->roll); g_m3_rx(t2, o->pitch); g_m3_mul(t2, t1, mm);  //pitch * roll
        g_m3_ry(t1, o->yaw); g_m3_mul(t1, mm, t2);
        for (int i = 0; i < 9; i++) mm[i] = t2[i];  //model: yaw * pitch * roll
        g_m3_mul(mcam, mm, mv);
        for (int i = 0; i < 9; i++) ms[i] = (int32_t)(((int64_t)mv[i] * o->scale) >> 12);
        int64_t dx = (int64_t)o->x - sc->cam_x, dy = (int64_t)o->y - sc->cam_y, dz = (int64_t)o->z - sc->cam_z;
        int32_t tx = (int32_t)((mcam[0] * dx + mcam[1] * dy + mcam[2] * dz) >> 14);
        int32_t ty = (int32_t)((mcam[3] * dx + mcam[4] * dy + mcam[5] * dz) >> 14);
        int32_t tz = (int32_t)((mcam[6] * dx + mcam[7] * dy + mcam[8] * dz) >> 14);
        int32_t lm[3];  //light in model space
        for (int i = 0; i < 3; i++) lm[i] = (mm[i] * lw[0] + mm[3 + i] * lw[1] + mm[6 + i] * lw[2]) >> 14;
        bool smooth = o->mode == ECCLES_GUI_3D_SMOOTH && m->vnormals;
        uint16_t base = gui->r3.nv;
        gui->r3.vbase[oi] = base;
        for (int v = 0; v < m->nverts; v++) {
            const int16_t *p = &m->verts[v * 3];
            int32_t X = ((ms[0] * p[0] + ms[1] * p[1] + ms[2] * p[2]) >> 14) + tx;
            int32_t Y = ((ms[3] * p[0] + ms[4] * p[1] + ms[5] * p[2]) >> 14) + ty;
            int32_t Z = ((ms[6] * p[0] + ms[7] * p[1] + ms[8] * p[2]) >> 14) + tz;
            eccles_gui_pv_t *pv = &gui->r3.pv[base + v];
            pv->ok = 0; pv->x = pv->y = 0; pv->iz = 0; pv->shade = 255;
            if (Z >= near_z) {
                int32_t sx = cx + (int32_t)(((int64_t)X * focal) / Z);
                int32_t sy = cy - (int32_t)(((int64_t)Y * focal) / Z);
                if (sx >= -G_RASTER_LIM && sx <= G_RASTER_LIM && sy >= -G_RASTER_LIM && sy <= G_RASTER_LIM) {
                    pv->ok = 1; pv->x = (int16_t)sx; pv->y = (int16_t)sy;
                    int64_t iz = ((int64_t)near_z * 65535) / Z;
                    pv->iz = (uint16_t)(iz > 65535 ? 65535 : iz);
                }
            }
            if (smooth) {
                const int8_t *vn = &m->vnormals[v * 3];
                pv->shade = g_shade((vn[0] * lm[0] + vn[1] * lm[1] + vn[2] * lm[2]) >> 7, amb);
            }
        }
        gui->r3.nv = (uint16_t)(gui->r3.nv + m->nverts);
        int nf = m->nfaces > 8191 ? 8191 : m->nfaces;
        for (int f = 0; f < nf; f++) {
            uint16_t ia = m->faces[f * 3], ib = m->faces[f * 3 + 1], ic = m->faces[f * 3 + 2];
            if (ia >= m->nverts || ib >= m->nverts || ic >= m->nverts) continue;
            const eccles_gui_pv_t *A = &gui->r3.pv[base + ia], *B = &gui->r3.pv[base + ib], *C = &gui->r3.pv[base + ic];
            if (!A->ok || !B->ok || !C->ok) { gui->r3.dropped++; continue; }
            int32_t area2 = ((int32_t)B->x - A->x) * ((int32_t)C->y - A->y) - ((int32_t)C->x - A->x) * ((int32_t)B->y - A->y);
            if (area2 == 0) continue;
            bool front = area2 > 0;
            if (!front && !(o->flags & ECCLES_GUI_3D_F_DOUBLE_SIDED) && !no_cull) continue;
            int mnx = G_MIN(A->x, G_MIN(B->x, C->x)), mxx = G_MAX(A->x, G_MAX(B->x, C->x));
            int mny = G_MIN(A->y, G_MIN(B->y, C->y)), mxy = G_MAX(A->y, G_MAX(B->y, C->y));
            if (mxx <= vx || mnx >= vx + vw || mxy <= vy || mny >= vy + vh) continue;
            if (gui->r3.nf >= ECCLES_GUI_3D_MAX_FACES) { gui->r3.dropped++; continue; }
            uint8_t shade = 255;
            if (o->mode != ECCLES_GUI_3D_UNLIT && o->mode != ECCLES_GUI_3D_WIRE) {
                int32_t n[3];
                if (m->normals) { n[0] = m->normals[f * 3]; n[1] = m->normals[f * 3 + 1]; n[2] = m->normals[f * 3 + 2]; }
                else g_face_normal(m, ia, ib, ic, n);
                int32_t d = (n[0] * lm[0] + n[1] * lm[1] + n[2] * lm[2]) >> 7;
                shade = g_shade(front ? d : -d, amb);
            }
            eccles_gui_pf_t *pf = &gui->r3.pf[gui->r3.nf++];
            pf->key = (int32_t)A->iz + B->iz + C->iz;
            pf->ref = (uint16_t)((uint16_t)oi << 13 | (uint16_t)f);
            pf->shade = shade; pf->pad = 0;
        }
    }
    gui->stats.faces_drawn = gui->r3.nf;
    gui->stats.faces_dropped = gui->r3.dropped;
}

//far (small key) first. shell sort: no recursion, no extra memory
static void g_sort_faces(eccles_gui_pf_t *a, int n) {
    static const int gaps[] = {301, 132, 57, 23, 10, 4, 1};
    for (size_t g = 0; g < sizeof gaps / sizeof gaps[0]; g++) {
        int gap = gaps[g];
        for (int i = gap; i < n; i++) {
            eccles_gui_pf_t t = a[i];
            int j = i;
            while (j >= gap && a[j - gap].key > t.key) { a[j] = a[j - gap]; j -= gap; }
            a[j] = t;
        }
    }
}

static int g_log2_pow2(int v) {
    if (v < 1 || v > 256 || (v & (v - 1))) return -1;
    int l = 0;
    while ((1 << l) < v) l++;
    return l;
}

G_HOT void eccles_gui_draw_view3d(eccles_gui_dc_t *dc, const eccles_gui_view3d_t *sc, int x, int y, int w, int h) {
    if (!dc || !sc || w <= 0 || h <= 0) return;
    eccles_gui_t *gui = dc->gui;
    eccles_gui_area_t vp = g_area_make(x, y, w, h);
    eccles_gui_area_t clip = g_area_isect(&dc->clip, &vp);
    if (g_area_empty(&clip)) return;
    eccles_gui_area_t saved = dc->clip;
    dc->clip = clip;
    if (!(sc->flags & ECCLES_GUI_3D_VIEW_TRANSPARENT)) eccles_gui_draw_fill(dc, clip.x1, clip.y1, clip.x2 - clip.x1, clip.y2 - clip.y1, sc->bg, 255);
#if ECCLES_GUI_3D_ZBUFFER
    bool use_z = dc->zbuf != NULL;
    if (use_z) {
        for (int j = clip.y1; j < clip.y2; j++)
            memset(dc->zbuf + (size_t)(j - dc->y0) * (size_t)dc->stride + (size_t)(clip.x1 - dc->x0), 0, (size_t)(clip.x2 - clip.x1) * 2u);
    }
#else
    bool use_z = false;
#endif
    if (gui->r3.scene != sc || gui->r3.frame != gui->frame || gui->r3.vx != x || gui->r3.vy != y || gui->r3.vw != w || gui->r3.vh != h) {
        g_prepare3d(gui, sc, x, y, w, h);
        if (!use_z) g_sort_faces(gui->r3.pf, gui->r3.nf);
    }
    for (int i = 0; i < gui->r3.nf; i++) {
        const eccles_gui_pf_t *pf = &gui->r3.pf[i];
        int oi = pf->ref >> 13, fi = pf->ref & 0x1FFF;
        const eccles_gui_obj3d_t *o = &sc->obj[oi];
        const eccles_gui_mesh_t *m = o->mesh;
        uint16_t base = gui->r3.vbase[oi];
        uint16_t ia = m->faces[fi * 3], ib = m->faces[fi * 3 + 1], ic = m->faces[fi * 3 + 2];
        const eccles_gui_pv_t *A = &gui->r3.pv[base + ia], *B = &gui->r3.pv[base + ib], *C = &gui->r3.pv[base + ic];
        int mny = G_MIN(A->y, G_MIN(B->y, C->y)), mxy = G_MAX(A->y, G_MAX(B->y, C->y));
        if (mxy < clip.y1 || mny >= clip.y2) continue;
        uint16_t color = m->colors ? m->colors[fi] : o->color;
        if (o->mode == ECCLES_GUI_3D_WIRE) {
            eccles_gui_draw_line(dc, A->x, A->y, B->x, B->y, 1, color, 255);
            eccles_gui_draw_line(dc, B->x, B->y, C->x, C->y, 1, color, 255);
            eccles_gui_draw_line(dc, C->x, C->y, A->x, A->y, 1, color, 255);
            continue;
        }
        g_tv_t a, b, c;
        memset(&a, 0, sizeof a); memset(&b, 0, sizeof b); memset(&c, 0, sizeof c);
        a.x = A->x; a.y = A->y; a.iz = A->iz; b.x = B->x; b.y = B->y; b.iz = B->iz; c.x = C->x; c.y = C->y; c.iz = C->iz;
        g_rp_t rp;
        memset(&rp, 0, sizeof rp);
        rp.opa = 255; rp.ztest = use_z ? 1 : 0; rp.color = color;
        int tl = 0;
#if ECCLES_GUI_3D_TEXTURES
        if (o->mode == ECCLES_GUI_3D_TEXTURED && o->tex && o->tex->data && m->uv && o->tex->fmt <= ECCLES_GUI_IMG_RGB565_KEY
            && g_log2_pow2(o->tex->w) >= 0 && g_log2_pow2(o->tex->h) >= 0) {
            rp.mode = G_R_TEX; rp.tex = (const uint16_t *)o->tex->data;
            rp.tw = (uint8_t)g_log2_pow2(o->tex->w); rp.th = (uint8_t)g_log2_pow2(o->tex->h);
            a.u = m->uv[ia * 2]; a.v = m->uv[ia * 2 + 1]; b.u = m->uv[ib * 2]; b.v = m->uv[ib * 2 + 1]; c.u = m->uv[ic * 2]; c.v = m->uv[ic * 2 + 1];
            a.shade = b.shade = c.shade = pf->shade;
            tl = 1;
        }
#endif
        if (!tl) {
            if (o->mode == ECCLES_GUI_3D_SMOOTH && m->vnormals) {
                rp.mode = G_R_SMOOTH;
                a.shade = A->shade; b.shade = B->shade; c.shade = C->shade;
            } else {
                rp.mode = G_R_FLAT;
                rp.color = g_scale5(color, ((uint32_t)pf->shade + 4u) >> 3);
            }
        }
        g_raster_tri(dc, &a, &b, &c, &rp);
    }
    dc->clip = saved;
}

#if ECCLES_GUI_HAS_WIDGETS && ECCLES_GUI_MAX_VIEWS3D > 0

eccles_gui_obj_t eccles_gui_view3d_create(eccles_gui_t *gui, eccles_gui_obj_t parent) {
    if (!gui || !gui->ready) return ECCLES_GUI_NO_OBJ;
    int slot = -1;
    for (int i = 0; i < ECCLES_GUI_MAX_VIEWS3D; i++) if (!gui->view_used[i]) { slot = i; break; }
    if (slot < 0) { g_err(gui, ECCLES_GUI_ERR_NO_MEMORY, "views3d"); return ECCLES_GUI_NO_OBJ; }
    eccles_gui_obj_t o = g_alloc(gui, ECCLES_GUI_W_VIEW3D, parent, ECCLES_GUI_STYLE_PANEL, ECCLES_GUI_F_CLICKABLE);
    if (o == ECCLES_GUI_NO_OBJ) return o;
    gui->view_used[slot] = 1;
    gui->w[o].u.v3d.view = (uint8_t)slot;
    gui->w[o].w = 160; gui->w[o].h = 120;
    eccles_gui_view3d_defaults(&gui->views[slot], 120);
    g_inv(gui, o);
    return o;
}

eccles_gui_view3d_t *eccles_gui_view3d_get(eccles_gui_t *gui, eccles_gui_obj_t obj) {
    if (!g_ok(gui, obj) || gui->w[obj].type != ECCLES_GUI_W_VIEW3D) return NULL;
    return &gui->views[gui->w[obj].u.v3d.view];
}
void eccles_gui_view3d_changed(eccles_gui_t *gui, eccles_gui_obj_t obj) { if (g_ok(gui, obj)) g_inv(gui, obj); }

static eccles_gui_obj3d_t *g_v3d_obj(eccles_gui_t *gui, eccles_gui_obj_t obj, int index) {
    eccles_gui_view3d_t *v = g_ok_t(gui, obj, ECCLES_GUI_W_VIEW3D) ? eccles_gui_view3d_get(gui, obj) : NULL;
    if (!v) return NULL;
    if (index < 0 || index >= ECCLES_GUI_3D_MAX_OBJECTS) { g_err(gui, ECCLES_GUI_ERR_BAD_ARG, "index"); return NULL; }
    return &v->obj[index];
}

int eccles_gui_view3d_add(eccles_gui_t *gui, eccles_gui_obj_t obj, const eccles_gui_mesh_t *mesh, eccles_gui_color_t color) {
    eccles_gui_view3d_t *v = g_ok_t(gui, obj, ECCLES_GUI_W_VIEW3D) ? eccles_gui_view3d_get(gui, obj) : NULL;
    if (!v) return -1;
    if (!mesh || !mesh->verts || !mesh->faces) { g_err(gui, ECCLES_GUI_ERR_BAD_ARG, "mesh"); return -1; }
    for (int i = 0; i < ECCLES_GUI_3D_MAX_OBJECTS; i++) {
        if (v->obj[i].mesh) continue;
        eccles_gui_obj3d_t *o = &v->obj[i];
        memset(o, 0, sizeof *o);
        o->mesh = mesh; o->color = color; o->scale = ECCLES_GUI_ONE;
        o->mode = ECCLES_GUI_3D_FLAT; o->flags = ECCLES_GUI_3D_F_VISIBLE;
        g_inv(gui, obj);
        return i;
    }
    g_err(gui, ECCLES_GUI_ERR_NO_MEMORY, "3d objects");
    return -1;
}

eccles_gui_status_t eccles_gui_view3d_set_transform(eccles_gui_t *gui, eccles_gui_obj_t obj, int index, int32_t x, int32_t y, int32_t z,
                                                    uint16_t yaw, uint16_t pitch, uint16_t roll, int32_t scale) {
    eccles_gui_obj3d_t *o = g_v3d_obj(gui, obj, index);
    if (!o) return ECCLES_GUI_FAILED;
    o->x = x; o->y = y; o->z = z; o->yaw = yaw; o->pitch = pitch; o->roll = roll; o->scale = scale;
    g_inv(gui, obj);
    return ECCLES_GUI_SUCCESSFUL;
}
eccles_gui_status_t eccles_gui_view3d_set_camera(eccles_gui_t *gui, eccles_gui_obj_t obj, int32_t x, int32_t y, int32_t z, uint16_t yaw, uint16_t pitch, int focal) {
    eccles_gui_view3d_t *v = g_ok_t(gui, obj, ECCLES_GUI_W_VIEW3D) ? eccles_gui_view3d_get(gui, obj) : NULL;
    if (!v) return ECCLES_GUI_FAILED;
    v->cam_x = x; v->cam_y = y; v->cam_z = z; v->cam_yaw = yaw; v->cam_pitch = pitch;
    v->focal = (int16_t)G_CLAMP(focal, 1, 30000);
    g_inv(gui, obj);
    return ECCLES_GUI_SUCCESSFUL;
}
eccles_gui_status_t eccles_gui_view3d_set_light(eccles_gui_t *gui, eccles_gui_obj_t obj, int dx, int dy, int dz, uint8_t ambient) {
    eccles_gui_view3d_t *v = g_ok_t(gui, obj, ECCLES_GUI_W_VIEW3D) ? eccles_gui_view3d_get(gui, obj) : NULL;
    if (!v) return ECCLES_GUI_FAILED;
    int64_t l2 = (int64_t)dx * dx + (int64_t)dy * dy + (int64_t)dz * dz;
    while (l2 > 0x7FFFFFFF) { dx /= 2; dy /= 2; dz /= 2; l2 = (int64_t)dx * dx + (int64_t)dy * dy + (int64_t)dz * dz; }
    int32_t len = (int32_t)g_isqrt((uint32_t)l2);
    if (len == 0) { v->light[0] = 0; v->light[1] = 0; v->light[2] = -127; }
    else { v->light[0] = (int8_t)(dx * 127 / len); v->light[1] = (int8_t)(dy * 127 / len); v->light[2] = (int8_t)(dz * 127 / len); }
    v->ambient = ambient;
    g_inv(gui, obj);
    return ECCLES_GUI_SUCCESSFUL;
}
eccles_gui_status_t eccles_gui_view3d_set_mode(eccles_gui_t *gui, eccles_gui_obj_t obj, int index, uint8_t mode) {
    eccles_gui_obj3d_t *o = g_v3d_obj(gui, obj, index);
    if (!o) return ECCLES_GUI_FAILED;
    if (mode > ECCLES_GUI_3D_TEXTURED) { g_err(gui, ECCLES_GUI_ERR_BAD_ARG, "mode"); return ECCLES_GUI_FAILED; }
    o->mode = mode;
    g_inv(gui, obj);
    return ECCLES_GUI_SUCCESSFUL;
}
eccles_gui_status_t eccles_gui_view3d_set_texture(eccles_gui_t *gui, eccles_gui_obj_t obj, int index, const eccles_gui_image_t *tex) {
    eccles_gui_obj3d_t *o = g_v3d_obj(gui, obj, index);
    if (!o) return ECCLES_GUI_FAILED;
    if (tex && (!tex->data || tex->fmt > ECCLES_GUI_IMG_RGB565_KEY || g_log2_pow2(tex->w) < 0 || g_log2_pow2(tex->h) < 0)) {
        g_err(gui, ECCLES_GUI_ERR_LIMIT, "texture must be rgb565, sides powers of two up to 256");
        return ECCLES_GUI_FAILED;
    }
    o->tex = tex;
    g_inv(gui, obj);
    return ECCLES_GUI_SUCCESSFUL;
}

#endif  //widgets + views
#endif  //ECCLES_GUI_HAS_3D

//10. generated data. tools/gen_data.py rewrites everything between the markers, run it after changing the generator

//>>> generated: sine (tools/gen_data.py)
static const int16_t g_sin_q[257] = {
    0, 201, 402, 603, 804, 1005, 1206, 1407, 1608, 1809, 2009, 2210,
    2410, 2611, 2811, 3012, 3212, 3412, 3612, 3811, 4011, 4210, 4410, 4609,
    4808, 5007, 5205, 5404, 5602, 5800, 5998, 6195, 6393, 6590, 6786, 6983,
    7179, 7375, 7571, 7767, 7962, 8157, 8351, 8545, 8739, 8933, 9126, 9319,
    9512, 9704, 9896, 10087, 10278, 10469, 10659, 10849, 11039, 11228, 11417, 11605,
    11793, 11980, 12167, 12353, 12539, 12725, 12910, 13094, 13279, 13462, 13645, 13828,
    14010, 14191, 14372, 14553, 14732, 14912, 15090, 15269, 15446, 15623, 15800, 15976,
    16151, 16325, 16499, 16673, 16846, 17018, 17189, 17360, 17530, 17700, 17869, 18037,
    18204, 18371, 18537, 18703, 18868, 19032, 19195, 19357, 19519, 19680, 19841, 20000,
    20159, 20317, 20475, 20631, 20787, 20942, 21096, 21250, 21403, 21554, 21705, 21856,
    22005, 22154, 22301, 22448, 22594, 22739, 22884, 23027, 23170, 23311, 23452, 23592,
    23731, 23870, 24007, 24143, 24279, 24413, 24547, 24680, 24811, 24942, 25072, 25201,
    25329, 25456, 25582, 25708, 25832, 25955, 26077, 26198, 26319, 26438, 26556, 26674,
    26790, 26905, 27019, 27133, 27245, 27356, 27466, 27575, 27683, 27790, 27896, 28001,
    28105, 28208, 28310, 28411, 28510, 28609, 28706, 28803, 28898, 28992, 29085, 29177,
    29268, 29358, 29447, 29534, 29621, 29706, 29791, 29874, 29956, 30037, 30117, 30195,
    30273, 30349, 30424, 30498, 30571, 30643, 30714, 30783, 30852, 30919, 30985, 31050,
    31113, 31176, 31237, 31297, 31356, 31414, 31470, 31526, 31580, 31633, 31685, 31736,
    31785, 31833, 31880, 31926, 31971, 32014, 32057, 32098, 32137, 32176, 32213, 32250,
    32285, 32318, 32351, 32382, 32412, 32441, 32469, 32495, 32521, 32545, 32567, 32589,
    32609, 32628, 32646, 32663, 32678, 32692, 32705, 32717, 32728, 32737, 32745, 32752,
    32757, 32761, 32765, 32766, 32767,
};
//<<< generated: sine

int16_t eccles_gui_sin(uint16_t angle) {
    uint32_t quad = angle >> 14, a = angle & 0x3FFFu;
    if (quad & 1u) a = 16384u - a;
    uint32_t idx = a >> 6, frac = a & 63u;
    int32_t v;
    if (idx >= 256) v = g_sin_q[256];
    else v = g_sin_q[idx] + (((g_sin_q[idx + 1] - g_sin_q[idx]) * (int32_t)frac) >> 6);
    return (int16_t)((quad & 2u) ? -v : v);
}
int16_t eccles_gui_cos(uint16_t angle) { return eccles_gui_sin((uint16_t)(angle + 16384u)); }

#if ECCLES_GUI_HAS_BUILTIN_FONT
//>>> generated: font (tools/gen_data.py)
static const uint8_t g_font5x8_bitmap[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x80, 0x00,
    0xA0, 0xA0, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x50, 0x50, 0xF8, 0x50, 0xF8, 0x50, 0x50, 0x00,
    0x20, 0x78, 0xA0, 0x70, 0x28, 0xF0, 0x20, 0x00,
    0xC0, 0xC8, 0x10, 0x20, 0x40, 0x98, 0x18, 0x00,
    0x40, 0xA0, 0xA0, 0x40, 0xA8, 0x90, 0x68, 0x00,
    0x60, 0x60, 0x40, 0x80, 0x00, 0x00, 0x00, 0x00,
    0x20, 0x40, 0x80, 0x80, 0x80, 0x40, 0x20, 0x00,
    0x80, 0x40, 0x20, 0x20, 0x20, 0x40, 0x80, 0x00,
    0x20, 0xA8, 0x70, 0xF8, 0x70, 0xA8, 0x20, 0x00,
    0x00, 0x20, 0x20, 0xF8, 0x20, 0x20, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x60, 0x60, 0x40, 0x80,
    0x00, 0x00, 0x00, 0xF8, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0xC0, 0x00,
    0x00, 0x08, 0x10, 0x20, 0x40, 0x80, 0x00, 0x00,
    0x70, 0x88, 0x98, 0xA8, 0xC8, 0x88, 0x70, 0x00,
    0x40, 0xC0, 0x40, 0x40, 0x40, 0x40, 0xE0, 0x00,
    0x70, 0x88, 0x08, 0x70, 0x80, 0x80, 0xF8, 0x00,
    0xF8, 0x08, 0x10, 0x30, 0x08, 0x88, 0x70, 0x00,
    0x10, 0x30, 0x50, 0x90, 0xF8, 0x10, 0x10, 0x00,
    0xF8, 0x80, 0xF0, 0x08, 0x08, 0x88, 0x70, 0x00,
    0x38, 0x40, 0x80, 0xF0, 0x88, 0x88, 0x70, 0x00,
    0xF8, 0x08, 0x08, 0x10, 0x20, 0x40, 0x80, 0x00,
    0x70, 0x88, 0x88, 0x70, 0x88, 0x88, 0x70, 0x00,
    0x70, 0x88, 0x88, 0x78, 0x08, 0x10, 0xE0, 0x00,
    0x00, 0x00, 0x80, 0x00, 0x80, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x40, 0x40, 0x80, 0x00,
    0x10, 0x20, 0x40, 0x80, 0x40, 0x20, 0x10, 0x00,
    0x00, 0x00, 0xF8, 0x00, 0xF8, 0x00, 0x00, 0x00,
    0x80, 0x40, 0x20, 0x10, 0x20, 0x40, 0x80, 0x00,
    0x70, 0x88, 0x08, 0x30, 0x20, 0x00, 0x20, 0x00,
    0x70, 0x88, 0xA8, 0xB8, 0xB0, 0x80, 0x78, 0x00,
    0x20, 0x50, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x00,
    0xF0, 0x88, 0x88, 0xF0, 0x88, 0x88, 0xF0, 0x00,
    0x70, 0x88, 0x80, 0x80, 0x80, 0x88, 0x70, 0x00,
    0xF0, 0x88, 0x88, 0x88, 0x88, 0x88, 0xF0, 0x00,
    0xF8, 0x80, 0x80, 0xF0, 0x80, 0x80, 0xF8, 0x00,
    0xF8, 0x80, 0x80, 0xF0, 0x80, 0x80, 0x80, 0x00,
    0x78, 0x88, 0x80, 0x80, 0x98, 0x88, 0x78, 0x00,
    0x88, 0x88, 0x88, 0xF8, 0x88, 0x88, 0x88, 0x00,
    0xE0, 0x40, 0x40, 0x40, 0x40, 0x40, 0xE0, 0x00,
    0x38, 0x10, 0x10, 0x10, 0x10, 0x90, 0x60, 0x00,
    0x88, 0x90, 0xA0, 0xC0, 0xA0, 0x90, 0x88, 0x00,
    0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xF8, 0x00,
    0x88, 0xD8, 0xA8, 0xA8, 0xA8, 0x88, 0x88, 0x00,
    0x88, 0x88, 0xC8, 0xA8, 0x98, 0x88, 0x88, 0x00,
    0x70, 0x88, 0x88, 0x88, 0x88, 0x88, 0x70, 0x00,
    0xF0, 0x88, 0x88, 0xF0, 0x80, 0x80, 0x80, 0x00,
    0x70, 0x88, 0x88, 0x88, 0xA8, 0x90, 0x68, 0x00,
    0xF0, 0x88, 0x88, 0xF0, 0xA0, 0x90, 0x88, 0x00,
    0x70, 0x88, 0x80, 0x70, 0x08, 0x88, 0x70, 0x00,
    0xF8, 0xA8, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00,
    0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x70, 0x00,
    0x88, 0x88, 0x88, 0x88, 0x88, 0x50, 0x20, 0x00,
    0x88, 0x88, 0x88, 0xA8, 0xA8, 0xA8, 0x50, 0x00,
    0x88, 0x88, 0x50, 0x20, 0x50, 0x88, 0x88, 0x00,
    0x88, 0x88, 0x50, 0x20, 0x20, 0x20, 0x20, 0x00,
    0xF8, 0x08, 0x10, 0x70, 0x40, 0x80, 0xF8, 0x00,
    0xF0, 0x80, 0x80, 0x80, 0x80, 0x80, 0xF0, 0x00,
    0x00, 0x80, 0x40, 0x20, 0x10, 0x08, 0x00, 0x00,
    0xF0, 0x10, 0x10, 0x10, 0x10, 0x10, 0xF0, 0x00,
    0x20, 0x50, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x00,
    0xC0, 0xC0, 0x40, 0x20, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x60, 0x10, 0x70, 0x90, 0x78, 0x00,
    0x80, 0x80, 0xB0, 0xC8, 0x88, 0xC8, 0xB0, 0x00,
    0x00, 0x00, 0x70, 0x88, 0x80, 0x88, 0x70, 0x00,
    0x08, 0x08, 0x68, 0x98, 0x88, 0x98, 0x68, 0x00,
    0x00, 0x00, 0x70, 0x88, 0xF8, 0x80, 0x70, 0x00,
    0x20, 0x50, 0x40, 0xE0, 0x40, 0x40, 0x40, 0x00,
    0x00, 0x00, 0x70, 0x98, 0x98, 0x68, 0x08, 0x70,
    0x80, 0x80, 0xB0, 0xC8, 0x88, 0x88, 0x88, 0x00,
    0x40, 0x00, 0xC0, 0x40, 0x40, 0x40, 0xE0, 0x00,
    0x10, 0x00, 0x10, 0x10, 0x10, 0x90, 0x60, 0x00,
    0x80, 0x80, 0x90, 0xA0, 0xC0, 0xA0, 0x90, 0x00,
    0xC0, 0x40, 0x40, 0x40, 0x40, 0x40, 0xE0, 0x00,
    0x00, 0x00, 0xD0, 0xA8, 0xA8, 0xA8, 0xA8, 0x00,
    0x00, 0x00, 0xB0, 0xC8, 0x88, 0x88, 0x88, 0x00,
    0x00, 0x00, 0x70, 0x88, 0x88, 0x88, 0x70, 0x00,
    0x00, 0x00, 0xB0, 0xC8, 0xC8, 0xB0, 0x80, 0x80,
    0x00, 0x00, 0x68, 0x98, 0x98, 0x68, 0x08, 0x08,
    0x00, 0x00, 0xB0, 0xC8, 0x80, 0x80, 0x80, 0x00,
    0x00, 0x00, 0x78, 0x80, 0x70, 0x08, 0xF0, 0x00,
    0x20, 0x20, 0xF8, 0x20, 0x20, 0x28, 0x10, 0x00,
    0x00, 0x00, 0x88, 0x88, 0x88, 0x98, 0x68, 0x00,
    0x00, 0x00, 0x88, 0x88, 0x88, 0x50, 0x20, 0x00,
    0x00, 0x00, 0x88, 0x88, 0xA8, 0xA8, 0x50, 0x00,
    0x00, 0x00, 0x88, 0x50, 0x20, 0x50, 0x88, 0x00,
    0x00, 0x00, 0x88, 0x88, 0x78, 0x08, 0x88, 0x70,
    0x00, 0x00, 0xF8, 0x10, 0x20, 0x40, 0xF8, 0x00,
    0x20, 0x40, 0x40, 0x80, 0x40, 0x40, 0x20, 0x00,
    0x80, 0x80, 0x80, 0x00, 0x80, 0x80, 0x80, 0x00,
    0x80, 0x40, 0x40, 0x20, 0x40, 0x40, 0x80, 0x00,
    0x40, 0xA8, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0xA0, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xA0, 0x40, 0xA0, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x40, 0xE0, 0x40, 0x00, 0x00,
};
static const eccles_gui_glyph_t g_font5x8_glyphs[] = {
    {0, 0, 8, 0, 0, 3},
    {8, 1, 8, 0, 0, 2},
    {16, 3, 8, 0, 0, 4},
    {24, 5, 8, 0, 0, 6},
    {32, 5, 8, 0, 0, 6},
    {40, 5, 8, 0, 0, 6},
    {48, 5, 8, 0, 0, 6},
    {56, 3, 8, 0, 0, 4},
    {64, 3, 8, 0, 0, 4},
    {72, 3, 8, 0, 0, 4},
    {80, 5, 8, 0, 0, 6},
    {88, 5, 8, 0, 0, 6},
    {96, 3, 8, 0, 0, 4},
    {104, 5, 8, 0, 0, 6},
    {112, 2, 8, 0, 0, 3},
    {120, 5, 8, 0, 0, 6},
    {128, 5, 8, 0, 0, 6},
    {136, 3, 8, 0, 0, 4},
    {144, 5, 8, 0, 0, 6},
    {152, 5, 8, 0, 0, 6},
    {160, 5, 8, 0, 0, 6},
    {168, 5, 8, 0, 0, 6},
    {176, 5, 8, 0, 0, 6},
    {184, 5, 8, 0, 0, 6},
    {192, 5, 8, 0, 0, 6},
    {200, 5, 8, 0, 0, 6},
    {208, 1, 8, 0, 0, 2},
    {216, 2, 8, 0, 0, 3},
    {224, 4, 8, 0, 0, 5},
    {232, 5, 8, 0, 0, 6},
    {240, 4, 8, 0, 0, 5},
    {248, 5, 8, 0, 0, 6},
    {256, 5, 8, 0, 0, 6},
    {264, 5, 8, 0, 0, 6},
    {272, 5, 8, 0, 0, 6},
    {280, 5, 8, 0, 0, 6},
    {288, 5, 8, 0, 0, 6},
    {296, 5, 8, 0, 0, 6},
    {304, 5, 8, 0, 0, 6},
    {312, 5, 8, 0, 0, 6},
    {320, 5, 8, 0, 0, 6},
    {328, 3, 8, 0, 0, 4},
    {336, 5, 8, 0, 0, 6},
    {344, 5, 8, 0, 0, 6},
    {352, 5, 8, 0, 0, 6},
    {360, 5, 8, 0, 0, 6},
    {368, 5, 8, 0, 0, 6},
    {376, 5, 8, 0, 0, 6},
    {384, 5, 8, 0, 0, 6},
    {392, 5, 8, 0, 0, 6},
    {400, 5, 8, 0, 0, 6},
    {408, 5, 8, 0, 0, 6},
    {416, 5, 8, 0, 0, 6},
    {424, 5, 8, 0, 0, 6},
    {432, 5, 8, 0, 0, 6},
    {440, 5, 8, 0, 0, 6},
    {448, 5, 8, 0, 0, 6},
    {456, 5, 8, 0, 0, 6},
    {464, 5, 8, 0, 0, 6},
    {472, 4, 8, 0, 0, 5},
    {480, 5, 8, 0, 0, 6},
    {488, 4, 8, 0, 0, 5},
    {496, 5, 8, 0, 0, 6},
    {504, 5, 8, 0, 0, 6},
    {512, 3, 8, 0, 0, 4},
    {520, 5, 8, 0, 0, 6},
    {528, 5, 8, 0, 0, 6},
    {536, 5, 8, 0, 0, 6},
    {544, 5, 8, 0, 0, 6},
    {552, 5, 8, 0, 0, 6},
    {560, 4, 8, 0, 0, 5},
    {568, 5, 8, 0, 0, 6},
    {576, 5, 8, 0, 0, 6},
    {584, 3, 8, 0, 0, 4},
    {592, 4, 8, 0, 0, 5},
    {600, 4, 8, 0, 0, 5},
    {608, 3, 8, 0, 0, 4},
    {616, 5, 8, 0, 0, 6},
    {624, 5, 8, 0, 0, 6},
    {632, 5, 8, 0, 0, 6},
    {640, 5, 8, 0, 0, 6},
    {648, 5, 8, 0, 0, 6},
    {656, 5, 8, 0, 0, 6},
    {664, 5, 8, 0, 0, 6},
    {672, 5, 8, 0, 0, 6},
    {680, 5, 8, 0, 0, 6},
    {688, 5, 8, 0, 0, 6},
    {696, 5, 8, 0, 0, 6},
    {704, 5, 8, 0, 0, 6},
    {712, 5, 8, 0, 0, 6},
    {720, 5, 8, 0, 0, 6},
    {728, 3, 8, 0, 0, 4},
    {736, 1, 8, 0, 0, 2},
    {744, 3, 8, 0, 0, 4},
    {752, 5, 8, 0, 0, 6},
    {760, 3, 8, 0, 0, 4},
    {768, 3, 8, 0, 0, 4},
    {776, 3, 8, 0, 0, 4},
};
static const eccles_gui_range_t g_font5x8_ranges[] = {
    {0x20, 95, 0}, {0xB0, 1, 95}, {0xD7, 1, 96}, {0x2022, 1, 97}
};
const eccles_gui_font_t eccles_gui_font_5x8 = {g_font5x8_glyphs, g_font5x8_bitmap, g_font5x8_ranges, 4, 9, 1};
//<<< generated: font
#endif

#if ECCLES_GUI_HAS_BUILTIN_MESHES
//>>> generated: meshes (tools/gen_data.py)
static const int16_t m_cube_v[] = {
    4096, 4096, -4096, 4096, 4096, 4096, 4096, -4096, 4096, 4096, -4096, -4096,
    -4096, 4096, 4096, -4096, 4096, -4096, -4096, -4096, -4096, -4096, -4096, 4096,
    -4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096, -4096, -4096, 4096, -4096,
    4096, -4096, 4096, -4096, -4096, 4096, -4096, -4096, -4096, 4096, -4096, -4096,
    4096, 4096, 4096, -4096, 4096, 4096, -4096, -4096, 4096, 4096, -4096, 4096,
    -4096, 4096, -4096, 4096, 4096, -4096, 4096, -4096, -4096, -4096, -4096, -4096,
};
static const uint16_t m_cube_f[] = {
    0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7,
    8, 9, 10, 8, 10, 11, 12, 13, 14, 12, 14, 15,
    16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
};
static const int8_t m_cube_n[] = {
    127, 0, 0, 127, 0, 0, -127, 0, 0, -127, 0, 0,
    0, 127, 0, 0, 127, 0, 0, -127, 0, 0, -127, 0,
    0, 0, 127, 0, 0, 127, 0, 0, -127, 0, 0, -127,
};
static const int8_t m_cube_vn[] = {
    127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0,
    -127, 0, 0, -127, 0, 0, -127, 0, 0, -127, 0, 0,
    0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0,
    0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127, 0,
    0, 0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127,
    0, 0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127,
};
static const uint8_t m_cube_uv[] = {
    0, 0, 255, 0, 255, 255, 0, 255, 0, 0, 255, 0, 255, 255, 0, 255,
    0, 0, 255, 0, 255, 255, 0, 255, 0, 0, 255, 0, 255, 255, 0, 255,
    0, 0, 255, 0, 255, 255, 0, 255, 0, 0, 255, 0, 255, 255, 0, 255,
};
const eccles_gui_mesh_t eccles_gui_mesh_cube = {m_cube_v, m_cube_f, m_cube_n, m_cube_vn, NULL, m_cube_uv, 24, 12};
static const int16_t m_pyramid_v[] = {
    -4096, -4096, -4096, 4096, -4096, -4096, 0, 4096, 0, 4096, -4096, -4096,
    4096, -4096, 4096, 0, 4096, 0, 4096, -4096, 4096, -4096, -4096, 4096,
    0, 4096, 0, -4096, -4096, 4096, -4096, -4096, -4096, 0, 4096, 0,
    -4096, -4096, -4096, 4096, -4096, -4096, 4096, -4096, 4096, -4096, -4096, 4096,
};
static const uint16_t m_pyramid_f[] = {
    0, 2, 1, 3, 5, 4, 6, 8, 7, 9, 11, 10,
    12, 13, 14, 12, 14, 15,
};
static const int8_t m_pyramid_n[] = {
    0, 57, -114, 114, 57, 0, 0, 57, 114, -114, 57, 0,
    0, -127, 0, 0, -127, 0,
};
static const int8_t m_pyramid_vn[] = {
    0, 57, -114, 0, 57, -114, 0, 57, -114, 114, 57, 0,
    114, 57, 0, 114, 57, 0, 0, 57, 114, 0, 57, 114,
    0, 57, 114, -114, 57, 0, -114, 57, 0, -114, 57, 0,
    0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127, 0,
};
static const uint8_t m_pyramid_uv[] = {
    0, 255, 255, 255, 128, 0, 0, 255, 255, 255, 128, 0, 0, 255, 255, 255,
    128, 0, 0, 255, 255, 255, 128, 0, 0, 0, 255, 0, 255, 255, 0, 255,
};
const eccles_gui_mesh_t eccles_gui_mesh_pyramid = {m_pyramid_v, m_pyramid_f, m_pyramid_n, m_pyramid_vn, NULL, m_pyramid_uv, 16, 6};
static const int16_t m_cylinder_v[] = {
    4096, 4096, 0, 4096, -4096, 0, 3784, 4096, 1567, 3784, -4096, 1567,
    2896, 4096, 2896, 2896, -4096, 2896, 1567, 4096, 3784, 1567, -4096, 3784,
    0, 4096, 4096, 0, -4096, 4096, -1567, 4096, 3784, -1567, -4096, 3784,
    -2896, 4096, 2896, -2896, -4096, 2896, -3784, 4096, 1567, -3784, -4096, 1567,
    -4096, 4096, 0, -4096, -4096, 0, -3784, 4096, -1567, -3784, -4096, -1567,
    -2896, 4096, -2896, -2896, -4096, -2896, -1567, 4096, -3784, -1567, -4096, -3784,
    0, 4096, -4096, 0, -4096, -4096, 1567, 4096, -3784, 1567, -4096, -3784,
    2896, 4096, -2896, 2896, -4096, -2896, 3784, 4096, -1567, 3784, -4096, -1567,
    4096, 4096, 0, 4096, -4096, 0, 0, 4096, 0, 4096, 4096, 0,
    3784, 4096, 1567, 2896, 4096, 2896, 1567, 4096, 3784, 0, 4096, 4096,
    -1567, 4096, 3784, -2896, 4096, 2896, -3784, 4096, 1567, -4096, 4096, 0,
    -3784, 4096, -1567, -2896, 4096, -2896, -1567, 4096, -3784, 0, 4096, -4096,
    1567, 4096, -3784, 2896, 4096, -2896, 3784, 4096, -1567, 0, -4096, 0,
    4096, -4096, 0, 3784, -4096, 1567, 2896, -4096, 2896, 1567, -4096, 3784,
    0, -4096, 4096, -1567, -4096, 3784, -2896, -4096, 2896, -3784, -4096, 1567,
    -4096, -4096, 0, -3784, -4096, -1567, -2896, -4096, -2896, -1567, -4096, -3784,
    0, -4096, -4096, 1567, -4096, -3784, 2896, -4096, -2896, 3784, -4096, -1567,
};
static const uint16_t m_cylinder_f[] = {
    0, 2, 1, 2, 3, 1, 2, 4, 3, 4, 5, 3,
    4, 6, 5, 6, 7, 5, 6, 8, 7, 8, 9, 7,
    8, 10, 9, 10, 11, 9, 10, 12, 11, 12, 13, 11,
    12, 14, 13, 14, 15, 13, 14, 16, 15, 16, 17, 15,
    16, 18, 17, 18, 19, 17, 18, 20, 19, 20, 21, 19,
    20, 22, 21, 22, 23, 21, 22, 24, 23, 24, 25, 23,
    24, 26, 25, 26, 27, 25, 26, 28, 27, 28, 29, 27,
    28, 30, 29, 30, 31, 29, 30, 32, 31, 32, 33, 31,
    34, 36, 35, 34, 37, 36, 34, 38, 37, 34, 39, 38,
    34, 40, 39, 34, 41, 40, 34, 42, 41, 34, 43, 42,
    34, 44, 43, 34, 45, 44, 34, 46, 45, 34, 47, 46,
    34, 48, 47, 34, 49, 48, 34, 50, 49, 34, 35, 50,
    51, 52, 53, 51, 53, 54, 51, 54, 55, 51, 55, 56,
    51, 56, 57, 51, 57, 58, 51, 58, 59, 51, 59, 60,
    51, 60, 61, 51, 61, 62, 51, 62, 63, 51, 63, 64,
    51, 64, 65, 51, 65, 66, 51, 66, 67, 51, 67, 52,
};
static const int8_t m_cylinder_n[] = {
    125, 0, 25, 125, 0, 25, 106, 0, 71, 106, 0, 71,
    71, 0, 106, 71, 0, 106, 25, 0, 125, 25, 0, 125,
    -25, 0, 125, -25, 0, 125, -71, 0, 106, -71, 0, 106,
    -106, 0, 71, -106, 0, 71, -125, 0, 25, -125, 0, 25,
    -125, 0, -25, -125, 0, -25, -106, 0, -71, -106, 0, -71,
    -71, 0, -106, -71, 0, -106, -25, 0, -125, -25, 0, -125,
    25, 0, -125, 25, 0, -125, 71, 0, -106, 71, 0, -106,
    106, 0, -71, 106, 0, -71, 125, 0, -25, 125, 0, -25,
    0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0,
    0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0,
    0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0,
    0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0,
    0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127, 0,
    0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127, 0,
    0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127, 0,
    0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127, 0,
};
static const int8_t m_cylinder_vn[] = {
    127, 0, 0, 127, 0, 0, 117, 0, 49, 117, 0, 49,
    90, 0, 90, 90, 0, 90, 49, 0, 117, 49, 0, 117,
    0, 0, 127, 0, 0, 127, -49, 0, 117, -49, 0, 117,
    -90, 0, 90, -90, 0, 90, -117, 0, 49, -117, 0, 49,
    -127, 0, 0, -127, 0, 0, -117, 0, -49, -117, 0, -49,
    -90, 0, -90, -90, 0, -90, -49, 0, -117, -49, 0, -117,
    0, 0, -127, 0, 0, -127, 49, 0, -117, 49, 0, -117,
    90, 0, -90, 90, 0, -90, 117, 0, -49, 117, 0, -49,
    127, 0, 0, 127, 0, 0, 0, 127, 0, 0, 127, 0,
    0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0,
    0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0,
    0, 127, 0, 0, 127, 0, 0, 127, 0, 0, 127, 0,
    0, 127, 0, 0, 127, 0, 0, 127, 0, 0, -127, 0,
    0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127, 0,
    0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127, 0,
    0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127, 0,
    0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127, 0,
};
static const uint8_t m_cylinder_uv[] = {
    0, 0, 0, 255, 16, 0, 16, 255, 32, 0, 32, 255, 48, 0, 48, 255,
    64, 0, 64, 255, 80, 0, 80, 255, 96, 0, 96, 255, 112, 0, 112, 255,
    128, 0, 128, 255, 143, 0, 143, 255, 159, 0, 159, 255, 175, 0, 175, 255,
    191, 0, 191, 255, 207, 0, 207, 255, 223, 0, 223, 255, 239, 0, 239, 255,
    255, 0, 255, 255, 128, 128, 255, 128, 245, 176, 218, 218, 176, 245, 128, 255,
    79, 245, 37, 218, 10, 176, 0, 128, 10, 79, 37, 37, 79, 10, 127, 0,
    176, 10, 218, 37, 245, 79, 128, 128, 255, 128, 245, 176, 218, 218, 176, 245,
    128, 255, 79, 245, 37, 218, 10, 176, 0, 128, 10, 79, 37, 37, 79, 10,
    127, 0, 176, 10, 218, 37, 245, 79,
};
const eccles_gui_mesh_t eccles_gui_mesh_cylinder = {m_cylinder_v, m_cylinder_f, m_cylinder_n, m_cylinder_vn, NULL, m_cylinder_uv, 68, 64};
static const int16_t m_sphere_v[] = {
    -2153, 3484, 0, 2153, 3484, 0, -2153, -3484, 0, 2153, -3484, 0,
    0, -2153, 3484, 0, 2153, 3484, 0, -2153, -3484, 0, 2153, -3484,
    3484, 0, -2153, 3484, 0, 2153, -3484, 0, -2153, -3484, 0, 2153,
    -3314, 2048, 1266, -2048, 1266, 3314, -1266, 3314, 2048, 1266, 3314, 2048,
    0, 4096, 0, 1266, 3314, -2048, -1266, 3314, -2048, -2048, 1266, -3314,
    -3314, 2048, -1266, -4096, 0, 0, 2048, 1266, 3314, 3314, 2048, 1266,
    -2048, -1266, 3314, 0, 0, 4096, -3314, -2048, -1266, -3314, -2048, 1266,
    0, 0, -4096, -2048, -1266, -3314, 3314, 2048, -1266, 2048, 1266, -3314,
    3314, -2048, 1266, 2048, -1266, 3314, 1266, -3314, 2048, -1266, -3314, 2048,
    0, -4096, 0, -1266, -3314, -2048, 1266, -3314, -2048, 2048, -1266, -3314,
    3314, -2048, -1266, 4096, 0, 0,
};
static const uint16_t m_sphere_f[] = {
    0, 12, 14, 11, 13, 12, 5, 14, 13, 12, 13, 14,
    0, 14, 16, 5, 15, 14, 1, 16, 15, 14, 15, 16,
    0, 16, 18, 1, 17, 16, 7, 18, 17, 16, 17, 18,
    0, 18, 20, 7, 19, 18, 10, 20, 19, 18, 19, 20,
    0, 20, 12, 10, 21, 20, 11, 12, 21, 20, 21, 12,
    1, 15, 23, 5, 22, 15, 9, 23, 22, 15, 22, 23,
    5, 13, 25, 11, 24, 13, 4, 25, 24, 13, 24, 25,
    11, 21, 27, 10, 26, 21, 2, 27, 26, 21, 26, 27,
    10, 19, 29, 7, 28, 19, 6, 29, 28, 19, 28, 29,
    7, 17, 31, 1, 30, 17, 8, 31, 30, 17, 30, 31,
    3, 32, 34, 9, 33, 32, 4, 34, 33, 32, 33, 34,
    3, 34, 36, 4, 35, 34, 2, 36, 35, 34, 35, 36,
    3, 36, 38, 2, 37, 36, 6, 38, 37, 36, 37, 38,
    3, 38, 40, 6, 39, 38, 8, 40, 39, 38, 39, 40,
    3, 40, 32, 8, 41, 40, 9, 32, 41, 40, 41, 32,
    4, 33, 25, 9, 22, 33, 5, 25, 22, 33, 22, 25,
    2, 35, 27, 4, 24, 35, 11, 27, 24, 35, 24, 27,
    6, 37, 29, 2, 26, 37, 10, 29, 26, 37, 26, 29,
    8, 39, 31, 6, 28, 39, 7, 31, 28, 39, 28, 31,
    9, 41, 23, 8, 30, 41, 1, 23, 30, 41, 30, 23,
};
static const int8_t m_sphere_n[] = {
    -74, 97, 36, -97, 36, 74, -36, 74, 97, -73, 73, 73,
    -37, 119, 22, 0, 96, 83, 37, 119, 22, 0, 119, 45,
    -37, 119, -22, 37, 119, -22, 0, 96, -83, 0, 119, -45,
    -74, 97, -36, -36, 74, -97, -97, 36, -74, -73, 73, -73,
    -96, 83, 0, -119, 22, -37, -119, 22, 37, -119, 45, 0,
    74, 97, 36, 36, 74, 97, 97, 36, 74, 73, 73, 73,
    -22, 37, 119, -83, 0, 96, -22, -37, 119, -45, 0, 119,
    -119, -22, 37, -119, -22, -37, -96, -83, 0, -119, -45, 0,
    -83, 0, -96, -22, 37, -119, -22, -37, -119, -45, 0, -119,
    36, 74, -97, 74, 97, -36, 97, 36, -74, 73, 73, -73,
    74, -97, 36, 97, -36, 74, 36, -74, 97, 73, -73, 73,
    37, -119, 22, 0, -96, 83, -37, -119, 22, 0, -119, 45,
    37, -119, -22, -37, -119, -22, 0, -96, -83, 0, -119, -45,
    74, -97, -36, 36, -74, -97, 97, -36, -74, 73, -73, -73,
    96, -83, 0, 119, -22, -37, 119, -22, 37, 119, -45, 0,
    22, -37, 119, 83, 0, 96, 22, 37, 119, 45, 0, 119,
    -74, -97, 36, -36, -74, 97, -97, -36, 74, -73, -73, 73,
    -36, -74, -97, -74, -97, -36, -97, -36, -74, -73, -73, -73,
    83, 0, -96, 22, -37, -119, 22, 37, -119, 45, 0, -119,
    119, 22, 37, 119, 22, -37, 96, 83, 0, 119, 45, 0,
};
static const int8_t m_sphere_vn[] = {
    -67, 108, 0, 67, 108, 0, -67, -108, 0, 67, -108, 0,
    0, -67, 108, 0, 67, 108, 0, -67, -108, 0, 67, -108,
    108, 0, -67, 108, 0, 67, -108, 0, -67, -108, 0, 67,
    -103, 64, 39, -64, 39, 103, -39, 103, 64, 39, 103, 64,
    0, 127, 0, 39, 103, -64, -39, 103, -64, -64, 39, -103,
    -103, 64, -39, -127, 0, 0, 64, 39, 103, 103, 64, 39,
    -64, -39, 103, 0, 0, 127, -103, -64, -39, -103, -64, 39,
    0, 0, -127, -64, -39, -103, 103, 64, -39, 64, 39, -103,
    103, -64, 39, 64, -39, 103, 39, -103, 64, -39, -103, 64,
    0, -127, 0, -39, -103, -64, 39, -103, -64, 64, -39, -103,
    103, -64, -39, 127, 0, 0,
};
const eccles_gui_mesh_t eccles_gui_mesh_sphere = {m_sphere_v, m_sphere_f, m_sphere_n, m_sphere_vn, NULL, NULL, 42, 80};
static const int16_t m_torus_v[] = {
    4096, 0, 0, 3736, 869, 0, 2867, 1229, 0, 1998, 869, 0,
    1638, 0, 0, 1998, -869, 0, 2867, -1229, 0, 3736, -869, 0,
    3547, 0, 2048, 3236, 869, 1868, 2483, 1229, 1434, 1731, 869, 999,
    1419, 0, 819, 1731, -869, 999, 2483, -1229, 1434, 3236, -869, 1868,
    2048, 0, 3547, 1868, 869, 3236, 1434, 1229, 2483, 999, 869, 1731,
    819, 0, 1419, 999, -869, 1731, 1434, -1229, 2483, 1868, -869, 3236,
    0, 0, 4096, 0, 869, 3736, 0, 1229, 2867, 0, 869, 1998,
    0, 0, 1638, 0, -869, 1998, 0, -1229, 2867, 0, -869, 3736,
    -2048, 0, 3547, -1868, 869, 3236, -1434, 1229, 2483, -999, 869, 1731,
    -819, 0, 1419, -999, -869, 1731, -1434, -1229, 2483, -1868, -869, 3236,
    -3547, 0, 2048, -3236, 869, 1868, -2483, 1229, 1434, -1731, 869, 999,
    -1419, 0, 819, -1731, -869, 999, -2483, -1229, 1434, -3236, -869, 1868,
    -4096, 0, 0, -3736, 869, 0, -2867, 1229, 0, -1998, 869, 0,
    -1638, 0, 0, -1998, -869, 0, -2867, -1229, 0, -3736, -869, 0,
    -3547, 0, -2048, -3236, 869, -1868, -2483, 1229, -1434, -1731, 869, -999,
    -1419, 0, -819, -1731, -869, -999, -2483, -1229, -1434, -3236, -869, -1868,
    -2048, 0, -3547, -1868, 869, -3236, -1434, 1229, -2483, -999, 869, -1731,
    -819, 0, -1419, -999, -869, -1731, -1434, -1229, -2483, -1868, -869, -3236,
    0, 0, -4096, 0, 869, -3736, 0, 1229, -2867, 0, 869, -1998,
    0, 0, -1638, 0, -869, -1998, 0, -1229, -2867, 0, -869, -3736,
    2048, 0, -3547, 1868, 869, -3236, 1434, 1229, -2483, 999, 869, -1731,
    819, 0, -1419, 999, -869, -1731, 1434, -1229, -2483, 1868, -869, -3236,
    3547, 0, -2048, 3236, 869, -1868, 2483, 1229, -1434, 1731, 869, -999,
    1419, 0, -819, 1731, -869, -999, 2483, -1229, -1434, 3236, -869, -1868,
};
static const uint16_t m_torus_f[] = {
    0, 9, 8, 0, 1, 9, 1, 10, 9, 1, 2, 10,
    2, 11, 10, 2, 3, 11, 3, 12, 11, 3, 4, 12,
    4, 13, 12, 4, 5, 13, 5, 14, 13, 5, 6, 14,
    6, 15, 14, 6, 7, 15, 7, 8, 15, 7, 0, 8,
    8, 17, 16, 8, 9, 17, 9, 18, 17, 9, 10, 18,
    10, 19, 18, 10, 11, 19, 11, 20, 19, 11, 12, 20,
    12, 21, 20, 12, 13, 21, 13, 22, 21, 13, 14, 22,
    14, 23, 22, 14, 15, 23, 15, 16, 23, 15, 8, 16,
    16, 25, 24, 16, 17, 25, 17, 26, 25, 17, 18, 26,
    18, 27, 26, 18, 19, 27, 19, 28, 27, 19, 20, 28,
    20, 29, 28, 20, 21, 29, 21, 30, 29, 21, 22, 30,
    22, 31, 30, 22, 23, 31, 23, 24, 31, 23, 16, 24,
    24, 33, 32, 24, 25, 33, 25, 34, 33, 25, 26, 34,
    26, 35, 34, 26, 27, 35, 27, 36, 35, 27, 28, 36,
    28, 37, 36, 28, 29, 37, 29, 38, 37, 29, 30, 38,
    30, 39, 38, 30, 31, 39, 31, 32, 39, 31, 24, 32,
    32, 41, 40, 32, 33, 41, 33, 42, 41, 33, 34, 42,
    34, 43, 42, 34, 35, 43, 35, 44, 43, 35, 36, 44,
    36, 45, 44, 36, 37, 45, 37, 46, 45, 37, 38, 46,
    38, 47, 46, 38, 39, 47, 39, 40, 47, 39, 32, 40,
    40, 49, 48, 40, 41, 49, 41, 50, 49, 41, 42, 50,
    42, 51, 50, 42, 43, 51, 43, 52, 51, 43, 44, 52,
    44, 53, 52, 44, 45, 53, 45, 54, 53, 45, 46, 54,
    46, 55, 54, 46, 47, 55, 47, 48, 55, 47, 40, 48,
    48, 57, 56, 48, 49, 57, 49, 58, 57, 49, 50, 58,
    50, 59, 58, 50, 51, 59, 51, 60, 59, 51, 52, 60,
    52, 61, 60, 52, 53, 61, 53, 62, 61, 53, 54, 62,
    54, 63, 62, 54, 55, 63, 55, 56, 63, 55, 48, 56,
    56, 65, 64, 56, 57, 65, 57, 66, 65, 57, 58, 66,
    58, 67, 66, 58, 59, 67, 59, 68, 67, 59, 60, 68,
    60, 69, 68, 60, 61, 69, 61, 70, 69, 61, 62, 70,
    62, 71, 70, 62, 63, 71, 63, 64, 71, 63, 56, 64,
    64, 73, 72, 64, 65, 73, 65, 74, 73, 65, 66, 74,
    66, 75, 74, 66, 67, 75, 67, 76, 75, 67, 68, 76,
    68, 77, 76, 68, 69, 77, 69, 78, 77, 69, 70, 78,
    70, 79, 78, 70, 71, 79, 71, 72, 79, 71, 64, 72,
    72, 81, 80, 72, 73, 81, 73, 82, 81, 73, 74, 82,
    74, 83, 82, 74, 75, 83, 75, 84, 83, 75, 76, 84,
    76, 85, 84, 76, 77, 85, 77, 86, 85, 77, 78, 86,
    78, 87, 86, 78, 79, 87, 79, 80, 87, 79, 72, 80,
    80, 89, 88, 80, 81, 89, 81, 90, 89, 81, 82, 90,
    82, 91, 90, 82, 83, 91, 83, 92, 91, 83, 84, 92,
    84, 93, 92, 84, 85, 93, 85, 94, 93, 85, 86, 94,
    86, 95, 94, 86, 87, 95, 87, 88, 95, 87, 80, 88,
    88, 1, 0, 88, 89, 1, 89, 2, 1, 89, 90, 2,
    90, 3, 2, 90, 91, 3, 91, 4, 3, 91, 92, 4,
    92, 5, 4, 92, 93, 5, 93, 6, 5, 93, 94, 6,
    94, 7, 6, 94, 95, 7, 95, 0, 7, 95, 88, 0,
};
static const int8_t m_torus_n[] = {
    113, 49, 30, 113, 49, 30, 47, 117, 13, 47, 117, 13,
    -47, 117, -13, -47, 117, -13, -113, 49, -30, -113, 49, -30,
    -113, -49, -30, -113, -49, -30, -47, -117, -13, -47, -117, -13,
    47, -117, 13, 47, -117, 13, 113, -49, 30, 113, -49, 30,
    83, 49, 83, 83, 49, 83, 34, 117, 34, 34, 117, 34,
    -34, 117, -34, -34, 117, -34, -83, 49, -83, -83, 49, -83,
    -83, -49, -83, -83, -49, -83, -34, -117, -34, -34, -117, -34,
    34, -117, 34, 34, -117, 34, 83, -49, 83, 83, -49, 83,
    30, 49, 113, 30, 49, 113, 13, 117, 47, 13, 117, 47,
    -13, 117, -47, -13, 117, -47, -30, 49, -113, -30, 49, -113,
    -30, -49, -113, -30, -49, -113, -13, -117, -47, -13, -117, -47,
    13, -117, 47, 13, -117, 47, 30, -49, 113, 30, -49, 113,
    -30, 49, 113, -30, 49, 113, -13, 117, 47, -13, 117, 47,
    13, 117, -47, 13, 117, -47, 30, 49, -113, 30, 49, -113,
    30, -49, -113, 30, -49, -113, 13, -117, -47, 13, -117, -47,
    -13, -117, 47, -13, -117, 47, -30, -49, 113, -30, -49, 113,
    -83, 49, 83, -83, 49, 83, -34, 117, 34, -34, 117, 34,
    34, 117, -34, 34, 117, -34, 83, 49, -83, 83, 49, -83,
    83, -49, -83, 83, -49, -83, 34, -117, -34, 34, -117, -34,
    -34, -117, 34, -34, -117, 34, -83, -49, 83, -83, -49, 83,
    -113, 49, 30, -113, 49, 30, -47, 117, 13, -47, 117, 13,
    47, 117, -13, 47, 117, -13, 113, 49, -30, 113, 49, -30,
    113, -49, -30, 113, -49, -30, 47, -117, -13, 47, -117, -13,
    -47, -117, 13, -47, -117, 13, -113, -49, 30, -113, -49, 30,
    -113, 49, -30, -113, 49, -30, -47, 117, -13, -47, 117, -13,
    47, 117, 13, 47, 117, 13, 113, 49, 30, 113, 49, 30,
    113, -49, 30, 113, -49, 30, 47, -117, 13, 47, -117, 13,
    -47, -117, -13, -47, -117, -13, -113, -49, -30, -113, -49, -30,
    -83, 49, -83, -83, 49, -83, -34, 117, -34, -34, 117, -34,
    34, 117, 34, 34, 117, 34, 83, 49, 83, 83, 49, 83,
    83, -49, 83, 83, -49, 83, 34, -117, 34, 34, -117, 34,
    -34, -117, -34, -34, -117, -34, -83, -49, -83, -83, -49, -83,
    -30, 49, -113, -30, 49, -113, -13, 117, -47, -13, 117, -47,
    13, 117, 47, 13, 117, 47, 30, 49, 113, 30, 49, 113,
    30, -49, 113, 30, -49, 113, 13, -117, 47, 13, -117, 47,
    -13, -117, -47, -13, -117, -47, -30, -49, -113, -30, -49, -113,
    30, 49, -113, 30, 49, -113, 13, 117, -47, 13, 117, -47,
    -13, 117, 47, -13, 117, 47, -30, 49, 113, -30, 49, 113,
    -30, -49, 113, -30, -49, 113, -13, -117, 47, -13, -117, 47,
    13, -117, -47, 13, -117, -47, 30, -49, -113, 30, -49, -113,
    83, 49, -83, 83, 49, -83, 34, 117, -34, 34, 117, -34,
    -34, 117, 34, -34, 117, 34, -83, 49, 83, -83, 49, 83,
    -83, -49, 83, -83, -49, 83, -34, -117, 34, -34, -117, 34,
    34, -117, -34, 34, -117, -34, 83, -49, -83, 83, -49, -83,
    113, 49, -30, 113, 49, -30, 47, 117, -13, 47, 117, -13,
    -47, 117, 13, -47, 117, 13, -113, 49, 30, -113, 49, 30,
    -113, -49, 30, -113, -49, 30, -47, -117, 13, -47, -117, 13,
    47, -117, -13, 47, -117, -13, 113, -49, -30, 113, -49, -30,
};
static const int8_t m_torus_vn[] = {
    127, 0, 0, 90, 90, 0, 0, 127, 0, -90, 90, 0,
    -127, 0, 0, -90, -90, 0, 0, -127, 0, 90, -90, 0,
    110, 0, 63, 78, 90, 45, 0, 127, 0, -78, 90, -45,
    -110, 0, -63, -78, -90, -45, 0, -127, 0, 78, -90, 45,
    64, 0, 110, 45, 90, 78, 0, 127, 0, -45, 90, -78,
    -64, 0, -110, -45, -90, -78, 0, -127, 0, 45, -90, 78,
    0, 0, 127, 0, 90, 90, 0, 127, 0, 0, 90, -90,
    0, 0, -127, 0, -90, -90, 0, -127, 0, 0, -90, 90,
    -63, 0, 110, -45, 90, 78, 0, 127, 0, 45, 90, -78,
    63, 0, -110, 45, -90, -78, 0, -127, 0, -45, -90, 78,
    -110, 0, 63, -78, 90, 45, 0, 127, 0, 78, 90, -45,
    110, 0, -63, 78, -90, -45, 0, -127, 0, -78, -90, 45,
    -127, 0, 0, -90, 90, 0, 0, 127, 0, 90, 90, 0,
    127, 0, 0, 90, -90, 0, 0, -127, 0, -90, -90, 0,
    -110, 0, -63, -78, 90, -45, 0, 127, 0, 78, 90, 45,
    110, 0, 63, 78, -90, 45, 0, -127, 0, -78, -90, -45,
    -64, 0, -110, -45, 90, -78, 0, 127, 0, 45, 90, 78,
    64, 0, 110, 45, -90, 78, 0, -127, 0, -45, -90, -78,
    0, 0, -127, 0, 90, -90, 0, 127, 0, 0, 90, 90,
    0, 0, 127, 0, -90, 90, 0, -127, 0, 0, -90, -90,
    64, 0, -110, 45, 90, -78, 0, 127, 0, -45, 90, 78,
    -64, 0, 110, -45, -90, 78, 0, -127, 0, 45, -90, -78,
    110, 0, -64, 78, 90, -45, 0, 127, 0, -78, 90, 45,
    -110, 0, 64, -78, -90, 45, 0, -127, 0, 78, -90, -45,
};
const eccles_gui_mesh_t eccles_gui_mesh_torus = {m_torus_v, m_torus_f, m_torus_n, m_torus_vn, NULL, NULL, 96, 192};
static const int16_t m_card_v[] = {
    -4096, 4096, 0, -2048, 4096, 0, 0, 4096, 0, 2048, 4096, 0,
    4096, 4096, 0, -4096, 2048, 0, -2048, 2048, 0, 0, 2048, 0,
    2048, 2048, 0, 4096, 2048, 0, -4096, 0, 0, -2048, 0, 0,
    0, 0, 0, 2048, 0, 0, 4096, 0, 0, -4096, -2048, 0,
    -2048, -2048, 0, 0, -2048, 0, 2048, -2048, 0, 4096, -2048, 0,
    -4096, -4096, 0, -2048, -4096, 0, 0, -4096, 0, 2048, -4096, 0,
    4096, -4096, 0,
};
static const uint16_t m_card_f[] = {
    0, 1, 6, 0, 6, 5, 1, 2, 7, 1, 7, 6,
    2, 3, 8, 2, 8, 7, 3, 4, 9, 3, 9, 8,
    5, 6, 11, 5, 11, 10, 6, 7, 12, 6, 12, 11,
    7, 8, 13, 7, 13, 12, 8, 9, 14, 8, 14, 13,
    10, 11, 16, 10, 16, 15, 11, 12, 17, 11, 17, 16,
    12, 13, 18, 12, 18, 17, 13, 14, 19, 13, 19, 18,
    15, 16, 21, 15, 21, 20, 16, 17, 22, 16, 22, 21,
    17, 18, 23, 17, 23, 22, 18, 19, 24, 18, 24, 23,
};
static const int8_t m_card_n[] = {
    0, 0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127,
    0, 0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127,
    0, 0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127,
    0, 0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127,
    0, 0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127,
    0, 0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127,
    0, 0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127,
    0, 0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127,
};
static const int8_t m_card_vn[] = {
    0, 0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127,
    0, 0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127,
    0, 0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127,
    0, 0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127,
    0, 0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127,
    0, 0, -127, 0, 0, -127, 0, 0, -127, 0, 0, -127,
    0, 0, -127,
};
static const uint8_t m_card_uv[] = {
    0, 0, 64, 0, 128, 0, 191, 0, 255, 0, 0, 64, 64, 64, 128, 64,
    191, 64, 255, 64, 0, 128, 64, 128, 128, 128, 191, 128, 255, 128, 0, 191,
    64, 191, 128, 191, 191, 191, 255, 191, 0, 255, 64, 255, 128, 255, 191, 255,
    255, 255,
};
const eccles_gui_mesh_t eccles_gui_mesh_card = {m_card_v, m_card_f, m_card_n, m_card_vn, NULL, m_card_uv, 25, 32};
//<<< generated: meshes
#endif
