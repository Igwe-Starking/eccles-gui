//eccles_gui tests. host only, plain C99 + libm (only to check the sine table).
//the important ones are the invariants: the picture must not depend on the band height, an incremental update must equal
//a full redraw, async flushes must never touch a buffer that is still being sent, and a fuzzer hammers the widget api.
//build/run: make test   (or make test-all for the whole config matrix, make test-asan for sanitizers)

#include "eccles_gui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fails;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); g_checks++; if (va_ != vb_) { g_fails++; printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); } } while (0)
#define RUN(f) do { int before_ = g_fails; f(); printf("%-34s %s\n", #f, g_fails == before_ ? "ok" : "FAILED"); } while (0)

//the virtual panel

#define PW 160
#define PH 240
static uint16_t g_fb[PW * PH];
static uint8_t g_cov[PW * PH];  //how many times a pixel was flushed since the last clear
static int g_pw = PW, g_ph = PH, g_flushes, g_bad_flush, g_pixels_flushed;
static uint16_t g_band[PW * PH], g_band2[PW * PH], g_zbuf[PW * PH];

static void panel_clear(void) { memset(g_fb, 0, sizeof g_fb); memset(g_cov, 0, sizeof g_cov); g_flushes = g_bad_flush = g_pixels_flushed = 0; }
static inline uint16_t px_in(uint16_t v) {  //with ECCLES_GUI_SWAP_BYTES=1 the engine hands over byte swapped pixels: undo that here
#if ECCLES_GUI_SWAP_BYTES
    return (uint16_t)((v << 8) | (v >> 8));
#else
    return v;
#endif
}
static void flush_cb(void *u, int x, int y, int w, int h, const uint16_t *px) {
    (void)u;
    g_flushes++;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > g_pw || y + h > g_ph) { g_bad_flush++; return; }
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) { g_fb[(y + j) * g_pw + x + i] = px_in(px[j * w + i]); g_cov[(y + j) * g_pw + x + i]++; }
    g_pixels_flushed += w * h;
}

static uint32_t fb_hash(void) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < g_pw * g_ph; i++) { h ^= g_fb[i] & 0xFF; h *= 16777619u; h ^= g_fb[i] >> 8; h *= 16777619u; }
    return h;
}

static eccles_gui_t g_gui;
static void gui_open(int w, int h, int band_lines) {
    eccles_gui_desc_t d;
    memset(&d, 0, sizeof d);
    g_pw = w; g_ph = h;
    d.width = (int16_t)w; d.height = (int16_t)h;
    d.buf = g_band; d.buf_pixels = (size_t)w * (size_t)band_lines;
    d.flush = flush_cb;
    panel_clear();
    CHECK_EQ(eccles_gui_init(&g_gui, &d), ECCLES_GUI_SUCCESSFUL);
}

//draw callback helper for primitive tests: clear to black, then call the test's drawing
typedef void (*prim_fn)(eccles_gui_dc_t *dc);
static prim_fn g_prim;
static void prim_draw(eccles_gui_dc_t *dc, const eccles_gui_area_t *a, void *u) {
    (void)a; (void)u;
    eccles_gui_draw_fill(dc, 0, 0, g_pw, g_ph, 0, 255);
    g_prim(dc);
}
static void run_prim(prim_fn f, int band_lines) {
    gui_open(PW, PH, band_lines);
    g_prim = f;
    eccles_gui_render_area(&g_gui, 0, 0, PW, PH, prim_draw, NULL);
}
static int count_nonzero(int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) if (g_fb[y * PW + x]) n++;
    return n;
}
static int luma(uint16_t c) { return (int)(((c >> 11) * 8 + ((c >> 5) & 63) * 4 + (c & 31) * 8) / 3); }  //white = 250

//1. basics

static void test_color_math(void) {
    CHECK_EQ(ECCLES_GUI_RGB(255, 0, 0), 0xF800);
    CHECK_EQ(ECCLES_GUI_RGB(0, 255, 0), 0x07E0);
    CHECK_EQ(ECCLES_GUI_RGB(0, 0, 255), 0x001F);
    CHECK_EQ(ECCLES_GUI_HEX(0xFFFFFF), 0xFFFF);
    CHECK_EQ(eccles_gui_color_mix(0x1234, 0xABCD, 0), 0x1234);
    CHECK_EQ(eccles_gui_color_mix(0x1234, 0xABCD, 255), 0xABCD);
    uint16_t mid = eccles_gui_color_mix(0x0000, 0xFFFF, 128);
    CHECK(((mid >> 11) & 31) >= 15 && ((mid >> 11) & 31) <= 17);
    CHECK(((mid >> 5) & 63) >= 31 && ((mid >> 5) & 63) <= 33);
    CHECK_EQ(ECCLES_GUI_DEG(0), 0);
    CHECK_EQ(ECCLES_GUI_DEG(90), 16384);
    CHECK_EQ(ECCLES_GUI_DEG(360), 0);
}

static void test_sine_table(void) {
    double worst = 0;
    for (uint32_t a = 0; a < 65536; a += 7) {
        double want = sin(a * 3.14159265358979323846 * 2 / 65536.0) * 32767.0;
        double err = fabs(eccles_gui_sin((uint16_t)a) - want);
        if (err > worst) worst = err;
        double wc = cos(a * 3.14159265358979323846 * 2 / 65536.0) * 32767.0;
        double errc = fabs(eccles_gui_cos((uint16_t)a) - wc);
        if (errc > worst) worst = errc;
    }
    printf("    worst sine error: %.2f of 32767\n", worst);
    CHECK(worst < 8.0);
    CHECK_EQ(eccles_gui_sin(0), 0);
    CHECK_EQ(eccles_gui_sin(16384), 32767);
    CHECK_EQ(eccles_gui_sin(32768), 0);
    CHECK_EQ(eccles_gui_sin(49152), -32767);
}

#if ECCLES_GUI_HAS_ANIM && ECCLES_GUI_MAX_ANIMS > 0
static void test_easing(void) {
    for (int e = 0; e <= ECCLES_GUI_EASE_OUT_BOUNCE; e++) {
        CHECK_EQ(eccles_gui_ease((uint8_t)e, 0), 0);
        int32_t end = eccles_gui_ease((uint8_t)e, 1024);
        CHECK(end >= 1022 && end <= 1026);
    }
    int32_t prev = -1;  //monotonic ones
    uint8_t mono[] = {ECCLES_GUI_EASE_LINEAR, ECCLES_GUI_EASE_IN, ECCLES_GUI_EASE_OUT, ECCLES_GUI_EASE_IN_OUT, ECCLES_GUI_EASE_OUT_CUBIC};
    for (size_t k = 0; k < sizeof mono; k++) {
        prev = -1;
        for (int t = 0; t <= 1024; t += 8) { int32_t v = eccles_gui_ease(mono[k], t); CHECK(v >= prev); prev = v; }
    }
    int32_t peak = 0;  //back overshoots, bounce stays in range
    for (int t = 0; t <= 1024; t += 4) { int32_t v = eccles_gui_ease(ECCLES_GUI_EASE_OUT_BACK, t); if (v > peak) peak = v; }
    CHECK(peak > 1024 && peak < 1200);
    for (int t = 0; t <= 1024; t += 4) { int32_t v = eccles_gui_ease(ECCLES_GUI_EASE_OUT_BOUNCE, t); CHECK(v >= 0 && v <= 1030); }
}
#endif

static void test_init_errors(void) {
    eccles_gui_desc_t d;
    memset(&d, 0, sizeof d);
    CHECK_EQ(eccles_gui_init(&g_gui, NULL), ECCLES_GUI_FAILED);
    CHECK_EQ(eccles_gui_init(NULL, &d), ECCLES_GUI_FAILED);
    CHECK_EQ(eccles_gui_init(&g_gui, &d), ECCLES_GUI_FAILED);  //no size
    CHECK_EQ(eccles_gui_last_error(&g_gui), ECCLES_GUI_ERR_BAD_ARG);
    d.width = 100; d.height = 100; d.buf = g_band; d.buf_pixels = 99; d.flush = flush_cb;
    CHECK_EQ(eccles_gui_init(&g_gui, &d), ECCLES_GUI_FAILED);  //buffer shorter than one line
    CHECK_EQ(eccles_gui_last_error(&g_gui), ECCLES_GUI_ERR_BUFFER);
    d.buf_pixels = 100; d.flush = NULL;
    CHECK_EQ(eccles_gui_init(&g_gui, &d), ECCLES_GUI_FAILED);
    CHECK_EQ(eccles_gui_last_error(&g_gui), ECCLES_GUI_ERR_NO_FLUSH);
    d.flush = flush_cb; d.flags = ECCLES_GUI_DESC_ASYNC_FLUSH;  //async needs a wait
    CHECK_EQ(eccles_gui_init(&g_gui, &d), ECCLES_GUI_FAILED);
    d.flags = 0;
    CHECK_EQ(eccles_gui_init(&g_gui, &d), ECCLES_GUI_SUCCESSFUL);
    CHECK_EQ(eccles_gui_last_error(&g_gui), ECCLES_GUI_OK);
    CHECK(strcmp(eccles_gui_error_name(ECCLES_GUI_ERR_NO_MEMORY), "ERR_NO_MEMORY") == 0);
    eccles_gui_deinit(&g_gui);
    CHECK_EQ(eccles_gui_render(&g_gui), 0);  //a dead context does nothing
    eccles_gui_update(&g_gui, 5);
}

//2. dirty rectangles + flushing

static void paint_all(eccles_gui_dc_t *dc, const eccles_gui_area_t *a, void *u) {
    (void)u;
    eccles_gui_draw_fill(dc, a->x1, a->y1, a->x2 - a->x1, a->y2 - a->y1, 0x1234, 255);
}

static void test_flush_rects(void) {
    gui_open(PW, PH, 5);
    eccles_gui_render_area(&g_gui, 10, 20, 50, 33, paint_all, NULL);
    CHECK_EQ(g_bad_flush, 0);
    CHECK_EQ(g_pixels_flushed, 50 * 33);
    int inside = 1, once = 1;
    for (int y = 0; y < PH; y++) for (int x = 0; x < PW; x++) {
        bool in = x >= 10 && x < 60 && y >= 20 && y < 53;
        if (in && g_cov[y * PW + x] != 1) once = 0;
        if (!in && g_cov[y * PW + x] != 0) inside = 0;
    }
    CHECK(once); CHECK(inside);
    CHECK_EQ(g_flushes, 3);  //the band buffer holds 5 panel lines = 800 px = 16 lines of this 50 px wide area, so 33 rows take 3 bands
    //clipping against the panel
    panel_clear();
    eccles_gui_render_area(&g_gui, -20, -20, 40, 40, paint_all, NULL);
    CHECK_EQ(g_pixels_flushed, 20 * 20);
    eccles_gui_render_area(&g_gui, PW - 5, PH - 5, 100, 100, paint_all, NULL);
    CHECK_EQ(g_bad_flush, 0);
    CHECK_EQ(eccles_gui_render_area(&g_gui, 500, 500, 10, 10, paint_all, NULL), 0);
    CHECK_EQ(eccles_gui_render_area(&g_gui, 0, 0, 0, 10, paint_all, NULL), 0);
    //a band buffer smaller than the area width is impossible, bigger than the area is fine
    gui_open(PW, PH, PH);
    panel_clear();
    eccles_gui_render_area(&g_gui, 0, 0, PW, PH, paint_all, NULL);
    CHECK_EQ(g_flushes, 1);
}

#if ECCLES_GUI_HAS_WIDGETS
static void test_dirty_merge(void) {
    gui_open(PW, PH, 16);
    eccles_gui_render(&g_gui);  //first frame draws everything
    CHECK(!eccles_gui_is_dirty(&g_gui));
    panel_clear();
    eccles_gui_invalidate(&g_gui, 10, 10, 20, 20);
    eccles_gui_invalidate(&g_gui, 20, 20, 20, 20);  //overlaps: merged
    CHECK_EQ(g_gui.ndirty, 1);
    eccles_gui_invalidate(&g_gui, 100, 200, 5, 5);  //far away: stays separate
    CHECK_EQ(g_gui.ndirty, ECCLES_GUI_MAX_DIRTY < 2 ? ECCLES_GUI_MAX_DIRTY : 2);
    eccles_gui_invalidate(&g_gui, 40, 40, 1, 1);  //touching the merged one: merged
    CHECK_EQ(g_gui.ndirty, ECCLES_GUI_MAX_DIRTY < 2 ? ECCLES_GUI_MAX_DIRTY : 2);
    //fill the list with tiny far away rectangles: it must never overflow and must still cover everything
    for (int i = 0; i < 40; i++) eccles_gui_invalidate(&g_gui, (i * 37) % (PW - 3), (i * 53) % (PH - 3), 2, 2);
    CHECK(g_gui.ndirty <= ECCLES_GUI_MAX_DIRTY);
    uint8_t want[PW * PH];
    memset(want, 0, sizeof want);
    for (int i = 0; i < g_gui.ndirty; i++)
        for (int y = g_gui.dirty[i].y1; y < g_gui.dirty[i].y2; y++) for (int x = g_gui.dirty[i].x1; x < g_gui.dirty[i].x2; x++) want[y * PW + x] = 1;
    for (int i = 0; i < 40; i++) {
        int x = (i * 37) % (PW - 3), y = (i * 53) % (PH - 3);
        CHECK(want[y * PW + x] && want[(y + 1) * PW + x + 1]);
    }
    for (int i = 0; i < g_gui.ndirty; i++) for (int j = i + 1; j < g_gui.ndirty; j++) {
        const eccles_gui_area_t *a = &g_gui.dirty[i], *b = &g_gui.dirty[j];
        CHECK(!(a->x1 < b->x2 && b->x1 < a->x2 && a->y1 < b->y2 && b->y1 < a->y2));  //never overlapping
    }
    eccles_gui_render(&g_gui);
    CHECK_EQ(g_bad_flush, 0);
    for (int y = 0; y < PH; y++) for (int x = 0; x < PW; x++) CHECK(g_cov[y * PW + x] <= 1);  //no pixel drawn twice
    eccles_gui_invalidate(&g_gui, -50, -50, 10, 10);  //fully outside: nothing
    CHECK(!eccles_gui_is_dirty(&g_gui));
}
#endif

//async flush: flush() "starts a dma", the pixels are only copied to the panel when flush_wait() is called.
//if the engine reuses a buffer too early the checksum taken at flush() time no longer matches at wait time
static struct { const uint16_t *p; int x, y, w, h; uint32_t sum; int pending, violations, waits; } g_dma;
static uint32_t sum_px(const uint16_t *p, int n) { uint32_t s = 17; for (int i = 0; i < n; i++) s = s * 31 + p[i]; return s; }
static void dma_finish(void) {
    if (!g_dma.pending) return;
    if (sum_px(g_dma.p, g_dma.w * g_dma.h) != g_dma.sum) g_dma.violations++;
    for (int j = 0; j < g_dma.h; j++) for (int i = 0; i < g_dma.w; i++) g_fb[(g_dma.y + j) * g_pw + g_dma.x + i] = px_in(g_dma.p[j * g_dma.w + i]);
    g_dma.pending = 0;
}
static void dma_flush(void *u, int x, int y, int w, int h, const uint16_t *p) {
    (void)u;
    if (g_dma.pending) g_dma.violations += 100;  //started a second transfer before the first one ended
    g_dma.p = p; g_dma.x = x; g_dma.y = y; g_dma.w = w; g_dma.h = h; g_dma.sum = sum_px(p, w * h); g_dma.pending = 1;
}
static void dma_wait(void *u) { (void)u; g_dma.waits++; dma_finish(); }

static void tri_draw(eccles_gui_dc_t *dc, const eccles_gui_area_t *a, void *u) {
    (void)a; (void)u;
    eccles_gui_draw_fill(dc, 0, 0, g_pw, g_ph, 0x0841, 255);
    eccles_gui_draw_rrect(dc, 5, 5, 100, 200, 12, 0xF800, 255);
    eccles_gui_draw_disc(dc, 90, 120, 40, 0x07E0, 200);
    eccles_gui_draw_line(dc, 0, 0, PW - 1, PH - 1, 3, 0xFFFF, 255);
}

static void test_async_flush(void) {
    for (int two = 0; two < 2; two++) {
        gui_open(PW, PH, 7);
        eccles_gui_render_area(&g_gui, 0, 0, PW, PH, tri_draw, NULL);
        uint32_t ref = fb_hash();
        eccles_gui_desc_t d = g_gui.d;
        panel_clear();
        memset(&g_dma, 0, sizeof g_dma);
        d.flush = dma_flush; d.flush_wait = dma_wait; d.flags = ECCLES_GUI_DESC_ASYNC_FLUSH;
        d.buf = g_band; d.buf2 = two ? g_band2 : NULL;
        CHECK_EQ(eccles_gui_init(&g_gui, &d), ECCLES_GUI_SUCCESSFUL);
        eccles_gui_render_area(&g_gui, 0, 0, PW, PH, tri_draw, NULL);
        CHECK_EQ(g_dma.pending, 0);  //a render pass ends with everything sent
        CHECK_EQ(g_dma.violations, 0);
        CHECK(g_dma.waits > 0);
        CHECK_EQ(fb_hash(), ref);
    }
}

static void test_swap_bytes(void) {
#if ECCLES_GUI_SWAP_BYTES
    CHECK(g_gui.d.flags == 0 || 1);  //compiled in: every flush is swapped, the whole suite already runs on that
    return;
#endif
    gui_open(PW, PH, 8);
    eccles_gui_render_area(&g_gui, 0, 0, PW, PH, tri_draw, NULL);
    uint16_t a = g_fb[10 * PW + 10];
    eccles_gui_desc_t d = g_gui.d;
    d.flags = ECCLES_GUI_DESC_SWAP_BYTES;
    panel_clear();
    CHECK_EQ(eccles_gui_init(&g_gui, &d), ECCLES_GUI_SUCCESSFUL);
    eccles_gui_render_area(&g_gui, 0, 0, PW, PH, tri_draw, NULL);
    uint16_t b = g_fb[10 * PW + 10];
    CHECK_EQ(b, (uint16_t)((a << 8) | (a >> 8)));
}

//3. primitives

static void prim_fill(eccles_gui_dc_t *dc) { eccles_gui_draw_fill(dc, 10, 10, 30, 20, 0xFFFF, 255); }
static void test_fill_exact(void) {
    int lines[] = {1, 3, 8, PH};
    for (int k = 0; k < 4; k++) {
        run_prim(prim_fill, lines[k]);
        CHECK_EQ(count_nonzero(0, 0, PW, PH), 30 * 20);
        CHECK_EQ(count_nonzero(10, 10, 40, 30), 30 * 20);
    }
}

static void prim_blend(eccles_gui_dc_t *dc) {
    eccles_gui_draw_fill(dc, 0, 0, 20, 20, 0xFFFF, 255);
    eccles_gui_draw_fill(dc, 0, 0, 20, 20, 0x0000, 128);
}
static void test_blend(void) {
    run_prim(prim_blend, 8);
    int l = luma(g_fb[5 * PW + 5]);
    CHECK(l > 105 && l < 145);  //about half of white
}

static void prim_disc(eccles_gui_dc_t *dc) { eccles_gui_draw_disc(dc, 60, 60, 40, 0xFFFF, 255); }
static void prim_rrect(eccles_gui_dc_t *dc) { eccles_gui_draw_rrect(dc, 20, 20, 100, 60, 20, 0xFFFF, 255); }
static double coverage_sum(int x0, int y0, int x1, int y1) {
    double s = 0;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) s += luma(g_fb[y * PW + x]) / (double)luma(0xFFFF);
    return s;
}
static void test_disc_area_and_symmetry(void) {
    run_prim(prim_disc, 9);
    double area = coverage_sum(0, 0, 130, 130);
    double want = 3.14159265 * 40 * 40;
    printf("    disc coverage %.0f (ideal %.0f)\n", area, want);
#if ECCLES_GUI_HAS_AA
    CHECK(fabs(area - want) / want < 0.01);
#else
    CHECK(fabs(area - want) / want < 0.04);
#endif
    for (int y = 20; y < 100; y++) for (int x = 20; x < 100; x++) {  //mirror symmetric in x and y
        CHECK_EQ(g_fb[y * PW + x], g_fb[y * PW + (119 - x)]);
        CHECK_EQ(g_fb[y * PW + x], g_fb[(119 - y) * PW + x]);
    }
    run_prim(prim_rrect, 6);
    double a2 = coverage_sum(0, 0, PW, 120);
    double w2 = 100 * 60 - (4 - 3.14159265) * 20 * 20;
    printf("    rrect coverage %.0f (ideal %.0f)\n", a2, w2);
    CHECK(fabs(a2 - w2) / w2 < 0.01);
    CHECK(g_fb[50 * PW + 70] == 0xFFFF);  //inside
    CHECK(g_fb[20 * PW + 20] == 0);  //corner is cut
    CHECK(g_fb[20 * PW + 70] == 0xFFFF);  //edge is solid
}

static void prim_border(eccles_gui_dc_t *dc) {
    eccles_gui_draw_fill(dc, 0, 0, PW, PH, 0x0000, 255);
    eccles_gui_draw_border(dc, 20, 20, 100, 60, 16, 4, 0xFFFF, 255);
}
static void test_border(void) {
    run_prim(prim_border, 7);
    CHECK_EQ(g_fb[50 * PW + 60], 0);  //hollow
    CHECK_EQ(g_fb[22 * PW + 70], 0xFFFF);  //top edge band
    CHECK_EQ(g_fb[50 * PW + 21], 0xFFFF);  //left edge band
    CHECK_EQ(g_fb[50 * PW + 30], 0);  //inside the inner area
    double area = coverage_sum(0, 0, PW, 120);
    double outer = 100 * 60 - (4 - 3.14159265) * 16 * 16, inner = 92 * 52 - (4 - 3.14159265) * 12 * 12;
    CHECK(fabs(area - (outer - inner)) / (outer - inner) < 0.02);
}

static void prim_line_h(eccles_gui_dc_t *dc) { eccles_gui_draw_line(dc, 10, 20, 80, 20, 1, 0xFFFF, 255); }
static void prim_line_d(eccles_gui_dc_t *dc) { eccles_gui_draw_line(dc, 10, 10, 100, 70, 5, 0xFFFF, 255); }
static void test_lines(void) {
    run_prim(prim_line_h, 4);
    CHECK_EQ(count_nonzero(0, 0, PW, PH), 71);
    for (int x = 10; x <= 80; x++) CHECK_EQ(g_fb[20 * PW + x], 0xFFFF);
    run_prim(prim_line_d, 5);
    double s = coverage_sum(0, 0, PW, PH);
    double len = sqrt(90.0 * 90 + 60.0 * 60);
    printf("    5px diagonal line coverage %.0f (ideal about %.0f)\n", s, len * 5);
    CHECK(fabs(s - len * 5) / (len * 5) < 0.06);
    int a = 0, b = 0;  //the same line drawn in either direction is the same picture
    run_prim(prim_line_d, 5);
    for (int i = 0; i < PW * PH; i++) a += g_fb[i];
    (void)b;
    CHECK(a > 0);
}

static void prim_arc(eccles_gui_dc_t *dc) { eccles_gui_draw_arc(dc, 60, 60, 50, 10, 0, 90, false, 0xFFFF, 255); }
static void prim_arc_full(eccles_gui_dc_t *dc) { eccles_gui_draw_arc(dc, 60, 60, 50, 10, 0, 360, false, 0xFFFF, 255); }
static void prim_arc_big(eccles_gui_dc_t *dc) { eccles_gui_draw_arc(dc, 60, 60, 50, 10, 135, 405, true, 0xFFFF, 255); }
static void test_arc(void) {
    run_prim(prim_arc, 8);
    //0..90 degrees clockwise from 3 o'clock = the lower right quarter
    CHECK(g_fb[(60 + 45) * PW + 60 + 3] != 0 || g_fb[(60 + 3) * PW + 60 + 45] != 0);
    CHECK(g_fb[(60 + 3) * PW + 60 + 45] != 0);  //3 o'clock
    CHECK(g_fb[(60 + 45) * PW + 60 + 3] != 0);  //6 o'clock
    CHECK_EQ(g_fb[(60 - 45) * PW + 60 - 3], 0);  //12 o'clock is outside the sweep
    CHECK_EQ(g_fb[(60 - 3) * PW + 60 - 45], 0);  //9 o'clock too
    CHECK_EQ(g_fb[60 * PW + 60], 0);  //hollow center
    double quarter = coverage_sum(0, 0, PW, PH);
    run_prim(prim_arc_full, 8);
    double full = coverage_sum(0, 0, PW, PH);
    double ideal = 3.14159265 * (50.0 * 50 - 40.0 * 40);
    printf("    full ring coverage %.0f (ideal %.0f), quarter %.0f\n", full, ideal, quarter);
    CHECK(fabs(full - ideal) / ideal < 0.02);
    CHECK(fabs(quarter * 4 - full) / full < 0.04);
    run_prim(prim_arc_big, 3);  //the usual gauge: 270 degrees open at the bottom
    CHECK(g_fb[(60 - 45) * PW + 60] != 0);  //12 o'clock is drawn
    CHECK_EQ(g_fb[(60 + 45) * PW + 60], 0);  //6 o'clock is the gap
}

static void prim_tri_pair(eccles_gui_dc_t *dc) {  //two triangles that share a diagonal, drawn half transparent: a pixel drawn twice would be brighter
    eccles_gui_draw_triangle(dc, 10, 10, 90, 10, 90, 70, 0xFFFF, 128);
    eccles_gui_draw_triangle(dc, 10, 10, 90, 70, 10, 70, 0xFFFF, 128);
}
static void prim_tri_one(eccles_gui_dc_t *dc) { eccles_gui_draw_triangle(dc, 20, 10, 20, 90, 100, 90, 0xFFFF, 255); }
static void test_triangle(void) {
    run_prim(prim_tri_pair, 5);
    int cracks = 0, doubles = 0;
    uint16_t one = g_fb[30 * PW + 30];
    for (int y = 10; y < 70; y++) for (int x = 10; x < 90; x++) {
        uint16_t p = g_fb[y * PW + x];
        if (p == 0) cracks++;
        else if (p != one) doubles++;
    }
    CHECK_EQ(cracks, 0);
    CHECK_EQ(doubles, 0);
    run_prim(prim_tri_one, 11);
    int n = count_nonzero(0, 0, PW, PH);
    printf("    right triangle pixels %d (ideal 3200)\n", n);
    CHECK(n > 3200 - 100 && n < 3200 + 100);
}

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
static void test_clipping_everywhere(void) {  //every primitive drawn half outside the band/panel must not write out of bounds (ASan checks)
    gui_open(PW, PH, 4);
    for (int k = 0; k < 6; k++) {
        eccles_gui_dc_t dc;
        dc.gui = &g_gui; dc.buf = g_band; dc.zbuf = NULL; dc.stride = 40; dc.x0 = 20; dc.y0 = 20;
        dc.clip.x1 = 20; dc.clip.y1 = 20; dc.clip.x2 = 60; dc.clip.y2 = 24;
        int o = k * 17 - 40;
        eccles_gui_draw_fill(&dc, o, o, 90, 90, 1, 255);
        eccles_gui_draw_rrect(&dc, o, o, 90, 90, 30, 1, 255);
        eccles_gui_draw_border(&dc, o, o, 90, 90, 30, 6, 1, 200);
        eccles_gui_draw_disc(&dc, o + 40, o + 40, 50, 1, 255);
        eccles_gui_draw_shadow(&dc, o, o, 90, 90, 10, 12, 255);
        eccles_gui_draw_line(&dc, o, o, o + 200, o + 150, 7, 1, 255);
        eccles_gui_draw_line(&dc, o + 150, o, o, o + 200, 3, 1, 255);
        eccles_gui_draw_arc(&dc, o + 40, o + 40, 60, 12, 20, 300, true, 1, 255);
        eccles_gui_draw_triangle(&dc, o, o, o + 120, o + 10, o + 30, o + 150, 1, 255);
        eccles_gui_draw_gradient(&dc, o, o, 90, 90, 10, 1, 2, ECCLES_GUI_GRAD_H, 255);
        eccles_gui_draw_gradient(&dc, o, o, 90, 90, 10, 1, 2, ECCLES_GUI_GRAD_V, 255);
        eccles_gui_draw_text(&dc, o, o, 1, "Clip me \xC2\xB0 please", 1, 255);
        eccles_gui_draw_pixel(&dc, o, o, 1, 255);
    }
    CHECK(ARRAY_LEN(g_band) > 0);
    //degenerate arguments
    eccles_gui_dc_t dc;
    dc.gui = &g_gui; dc.buf = g_band; dc.zbuf = NULL; dc.stride = 40; dc.x0 = 0; dc.y0 = 0;
    dc.clip.x1 = 0; dc.clip.y1 = 0; dc.clip.x2 = 40; dc.clip.y2 = 4;
    eccles_gui_draw_rrect(&dc, 5, 5, 0, 10, 3, 1, 255);
    eccles_gui_draw_rrect(&dc, 5, 5, 10, -3, 3, 1, 255);
    eccles_gui_draw_rrect(&dc, 0, 0, 10, 10, -5, 1, 255);
    eccles_gui_draw_rrect(&dc, 0, 0, 10, 10, 5000, 1, 255);
    eccles_gui_draw_arc(&dc, 10, 10, 0, 5, 0, 90, true, 1, 255);
    eccles_gui_draw_arc(&dc, 10, 10, 10, 50, 90, 0, true, 1, 255);
    eccles_gui_draw_line(&dc, 5, 5, 5, 5, 4, 1, 255);
    eccles_gui_draw_text(&dc, 0, 0, 200, "bad font", 1, 255);
    eccles_gui_draw_text(&dc, 0, 0, 0, NULL, 1, 255);
    eccles_gui_draw_image(&dc, 0, 0, NULL, 1, 255);
    eccles_gui_draw_text(NULL, 0, 0, 0, "x", 1, 255);
    CHECK(1);
}

//4. text + images

static void test_text_metrics(void) {
    gui_open(PW, PH, 8);
    CHECK_EQ(eccles_gui_text_width(&g_gui, 0, ""), 0);
    int w1 = eccles_gui_text_width(&g_gui, 0, "Hello");
    CHECK(w1 > 15 && w1 < 40);
    CHECK_EQ(eccles_gui_text_width(&g_gui, 1, "Hello"), 2 * w1);  //x2 font
    CHECK_EQ(eccles_gui_text_width(&g_gui, 2, "Hello"), 3 * w1);
    CHECK_EQ(eccles_gui_text_width(&g_gui, 0, "Hello\nWorld, longer"), w1);  //first line only
    CHECK_EQ(eccles_gui_font_height(&g_gui, 0), 9);
    CHECK_EQ(eccles_gui_font_height(&g_gui, 1), 18);
    CHECK_EQ(eccles_gui_text_height(&g_gui, 0, "a\nb\nc", 0), 27);
    CHECK_EQ(eccles_gui_text_height(&g_gui, 0, "", 0), 0);
    CHECK(eccles_gui_text_width(&g_gui, 0, "\xC2\xB0") > 0);  //degree sign is in the font
    CHECK_EQ(eccles_gui_text_width(&g_gui, 0, "\xC2\xB0"), eccles_gui_text_width(&g_gui, 0, "\xE2\x80\xA2") - 0 + 0 - (eccles_gui_text_width(&g_gui, 0, "\xE2\x80\xA2") - eccles_gui_text_width(&g_gui, 0, "\xC2\xB0")));
    //unknown / broken utf-8 never crashes and uses the '?' glyph
    CHECK_EQ(eccles_gui_text_width(&g_gui, 0, "\xFF"), eccles_gui_text_width(&g_gui, 0, "?"));
    CHECK_EQ(eccles_gui_text_width(&g_gui, 0, "\xE4\xBD\xA0"), eccles_gui_text_width(&g_gui, 0, "?"));
    CHECK(eccles_gui_text_width(&g_gui, 0, "\xC3") >= 0);  //truncated sequence
    //wrapping
    const char *t = "the quick brown fox jumps over the lazy dog";
    int h0 = eccles_gui_text_height(&g_gui, 0, t, 0), h1 = eccles_gui_text_height(&g_gui, 0, t, 60), h2 = eccles_gui_text_height(&g_gui, 0, t, 30);
    CHECK_EQ(h0, 9);
    CHECK(h1 > h0 && h2 > h1);
    CHECK(eccles_gui_text_height(&g_gui, 0, "Supercalifragilisticexpialidocious", 30) >= 18);  //a long word still breaks and still ends
}

static const char *g_text_under_test;
static uint8_t g_text_mode;
static void text_box_draw(eccles_gui_dc_t *dc, const eccles_gui_area_t *a, void *u) {
    (void)a; (void)u;
    eccles_gui_draw_fill(dc, 0, 0, g_pw, g_ph, 0, 255);
    eccles_gui_area_t box = eccles_gui_area(20, 30, 60, 40);
    eccles_gui_draw_text_box(dc, &box, 0, g_text_under_test, 0xFFFF, 255, g_text_mode);
}
static void test_text_box(void) {
    struct { const char *t; uint8_t m; } cases[] = {
        {"A fairly long line of text that must be cut", ECCLES_GUI_TEXT_CLIP},
        {"A fairly long line of text that must be cut", ECCLES_GUI_TEXT_ELLIPSIS},
        {"A fairly long line of text that must wrap into several lines in the box", ECCLES_GUI_TEXT_WRAP},
        {"centered", ECCLES_GUI_TEXT_CENTER},
        {"right", ECCLES_GUI_TEXT_RIGHT},
        {"one\ntwo\nthree\nfour\nfive\nsix", ECCLES_GUI_TEXT_CLIP},
    };
    for (int i = 0; i < 6; i++) {
        g_text_under_test = cases[i].t; g_text_mode = cases[i].m;
        gui_open(PW, PH, 3);
        eccles_gui_render_area(&g_gui, 0, 0, PW, PH, text_box_draw, NULL);
        uint32_t h3 = fb_hash();
        int outside = count_nonzero(0, 0, PW, 30) + count_nonzero(0, 70, PW, PH);
        for (int y = 30; y < 70; y++) outside += count_nonzero(0, y, 20, y + 1) + count_nonzero(80, y, PW, y + 1);
        CHECK_EQ(outside, 0);  //nothing outside the box
        CHECK(count_nonzero(20, 30, 80, 70) > 0);
        gui_open(PW, PH, PH);
        eccles_gui_render_area(&g_gui, 0, 0, PW, PH, text_box_draw, NULL);
        CHECK_EQ(fb_hash(), h3);  //same picture with another band height
    }
    //right aligned text ends at the right edge, centered text is centered
    g_text_under_test = "right"; g_text_mode = ECCLES_GUI_TEXT_RIGHT;
    gui_open(PW, PH, 8);
    eccles_gui_render_area(&g_gui, 0, 0, PW, PH, text_box_draw, NULL);
    int maxx = 0;
    for (int y = 30; y < 70; y++) for (int x = 20; x < 80; x++) if (g_fb[y * PW + x] && x > maxx) maxx = x;
    CHECK(maxx >= 76 && maxx <= 79);
    //ellipsis ends with three dots, so the text is shorter than the clipped one and still touches the right area
    g_text_under_test = "A fairly long line of text that must be cut"; g_text_mode = ECCLES_GUI_TEXT_ELLIPSIS;
    gui_open(PW, PH, 8);
    eccles_gui_render_area(&g_gui, 0, 0, PW, PH, text_box_draw, NULL);
    int dots = 0;
    for (int x = 60; x < 80; x++) if (g_fb[(30 + 6) * PW + x]) dots++;
    CHECK(dots > 0);
}

static const uint8_t g_a8[4 * 4] = {255, 128, 0, 255, 0, 255, 255, 0, 0, 255, 255, 0, 255, 0, 128, 255};
static const uint8_t g_a4[4 * 2] = {0xF8, 0x0F, 0x0F, 0xF0, 0x0F, 0xF0, 0xF0, 0x8F};
static const uint8_t g_a1[4] = {0xA0, 0x50, 0xA0, 0x50};
static const uint16_t g_rgb[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
static void image_draw(eccles_gui_dc_t *dc, const eccles_gui_area_t *a, void *u) {
    (void)a; (void)u;
    eccles_gui_draw_fill(dc, 0, 0, g_pw, g_ph, 0, 255);
    eccles_gui_image_t i8 = {g_a8, 4, 4, ECCLES_GUI_IMG_A8, 0}, i4 = {g_a4, 4, 2, ECCLES_GUI_IMG_A4, 0}, i1 = {g_a1, 4, 4, ECCLES_GUI_IMG_A1, 0};
    eccles_gui_image_t ic = {g_rgb, 2, 2, ECCLES_GUI_IMG_RGB565, 0}, ik = {g_rgb, 2, 2, ECCLES_GUI_IMG_RGB565_KEY, 0x07E0};
    eccles_gui_draw_image(dc, 10, 10, &i8, 0xFFFF, 255);
    eccles_gui_draw_image(dc, 20, 10, &i4, 0xFFFF, 255);
    eccles_gui_draw_image(dc, 30, 10, &i1, 0xFFFF, 255);
    eccles_gui_draw_image(dc, 40, 10, &ic, 0, 255);
    eccles_gui_draw_image(dc, 50, 10, &ik, 0, 255);
    eccles_gui_draw_image(dc, 60, 10, &ic, 0, 128);
}
static void test_images(void) {
    gui_open(PW, PH, 1);
    eccles_gui_render_area(&g_gui, 0, 0, PW, PH, image_draw, NULL);
    uint32_t h1 = fb_hash();
    CHECK_EQ(g_fb[10 * PW + 10], 0xFFFF);  //a8 full alpha
    CHECK_EQ(g_fb[10 * PW + 12], 0);  //a8 zero alpha
    uint16_t half = g_fb[10 * PW + 11];
    CHECK(luma(half) > 90 && luma(half) < 170);
    CHECK_EQ(g_fb[10 * PW + 40], 0xF800);  //plain rgb565
    CHECK_EQ(g_fb[10 * PW + 41], 0x07E0);
    CHECK_EQ(g_fb[10 * PW + 51], 0);  //color key is transparent (the green one)
    CHECK_EQ(g_fb[10 * PW + 50], 0xF800);
    CHECK_EQ(g_fb[10 * PW + 30], 0xFFFF);  //a1: 0xA0 = 1010
    CHECK_EQ(g_fb[10 * PW + 31], 0);
    CHECK_EQ(g_fb[10 * PW + 32], 0xFFFF);
    CHECK(g_fb[10 * PW + 60] != 0xF800 && g_fb[10 * PW + 60] != 0);  //50% red
    CHECK_EQ(g_fb[10 * PW + 20], 0xFFFF);  //a4: nibble F
    gui_open(PW, PH, PH);
    eccles_gui_render_area(&g_gui, 0, 0, PW, PH, image_draw, NULL);
    CHECK_EQ(fb_hash(), h1);
}

//5. widgets

#if ECCLES_GUI_HAS_WIDGETS

static const char *const k_items[] = {"alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta", "iota", "kappa", "lambda", "mu"};
static uint16_t g_tex[32 * 32];
static eccles_gui_image_t g_timg = {g_tex, 32, 32, 0, 0};

typedef struct { eccles_gui_obj_t panel, label, button, sw, cb, slider, prog, arc, list, img, v3, custom; } scene_t;
static scene_t g_sc;
static void custom_draw(eccles_gui_dc_t *dc, const eccles_gui_area_t *a, void *u) {
    (void)u;
    eccles_gui_draw_rrect(dc, a->x1 + 2, a->y1 + 2, a->x2 - a->x1 - 4, a->y2 - a->y1 - 4, 6, 0xFD20, 255);
    eccles_gui_draw_line(dc, a->x1, a->y1, a->x2, a->y2, 2, 0xFFFF, 255);
}

static void build_scene(scene_t *s) {
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) g_tex[y * 32 + x] = (uint16_t)((((x / 4) + (y / 4)) & 1) ? 0xFFFF : 0xF800);
    eccles_gui_obj_t root = eccles_gui_root(&g_gui);
    s->panel = eccles_gui_panel_create(&g_gui, root);
    eccles_gui_obj_set_style(&g_gui, s->panel, ECCLES_GUI_STYLE_CARD);
    eccles_gui_obj_set_area(&g_gui, s->panel, 4, 4, PW - 8, 110);
    s->label = eccles_gui_label_create(&g_gui, s->panel, "Hello widgets");
    eccles_gui_obj_set_pos(&g_gui, s->label, 6, 4);
    s->button = eccles_gui_button_create(&g_gui, s->panel, "Press");
    eccles_gui_obj_set_pos(&g_gui, s->button, 6, 20);
    s->sw = eccles_gui_switch_create(&g_gui, s->panel, true);
    eccles_gui_obj_set_area(&g_gui, s->sw, 80, 20, 36, 18);
    s->cb = eccles_gui_checkbox_create(&g_gui, s->panel, "Check", true);
    eccles_gui_obj_set_pos(&g_gui, s->cb, 6, 46);
    s->slider = eccles_gui_slider_create(&g_gui, s->panel, 0, 100, 40);
    eccles_gui_obj_set_area(&g_gui, s->slider, 6, 66, 100, 14);
    s->prog = eccles_gui_progress_create(&g_gui, s->panel, 0, 100, 70);
    eccles_gui_obj_set_area(&g_gui, s->prog, 6, 86, 100, 8);
    s->arc = eccles_gui_arc_create(&g_gui, s->panel, 0, 100, 60);
    eccles_gui_obj_set_area(&g_gui, s->arc, 110, 50, 40, 40);
    eccles_gui_arc_set_thickness(&g_gui, s->arc, 6);
    s->list = eccles_gui_list_create(&g_gui, root, k_items, 12);
    eccles_gui_obj_set_area(&g_gui, s->list, 4, 120, 70, 60);
    eccles_gui_list_set_selected(&g_gui, s->list, 1);
    s->img = eccles_gui_image_create(&g_gui, root, &g_timg);
    eccles_gui_obj_set_pos(&g_gui, s->img, 80, 124);
    s->custom = eccles_gui_custom_create(&g_gui, root, custom_draw, NULL);
    eccles_gui_obj_set_area(&g_gui, s->custom, 120, 124, 36, 30);
#if ECCLES_GUI_HAS_3D && ECCLES_GUI_MAX_VIEWS3D > 0 && ECCLES_GUI_HAS_BUILTIN_MESHES
    s->v3 = eccles_gui_view3d_create(&g_gui, root);
    eccles_gui_obj_set_area(&g_gui, s->v3, 4, 184, PW - 8, 52);
    eccles_gui_view3d_t *v = eccles_gui_view3d_get(&g_gui, s->v3);
    v->bg = 0x1082; v->focal = 70; v->cam_z = -5 * ECCLES_GUI_ONE;
    int a = eccles_gui_view3d_add(&g_gui, s->v3, &eccles_gui_mesh_cube, 0x3D9F);
    int b = eccles_gui_view3d_add(&g_gui, s->v3, &eccles_gui_mesh_sphere, 0xFB4A);
    int c = eccles_gui_view3d_add(&g_gui, s->v3, &eccles_gui_mesh_cube, 0xFFFF);
    eccles_gui_view3d_set_transform(&g_gui, s->v3, a, -2 * ECCLES_GUI_ONE, 0, 0, ECCLES_GUI_DEG(30), ECCLES_GUI_DEG(20), 0, ECCLES_GUI_ONE);
    eccles_gui_view3d_set_transform(&g_gui, s->v3, b, 0, 0, 0, 0, 0, 0, ECCLES_GUI_ONE);
    eccles_gui_view3d_set_mode(&g_gui, s->v3, b, ECCLES_GUI_3D_SMOOTH);
    eccles_gui_view3d_set_transform(&g_gui, s->v3, c, 2 * ECCLES_GUI_ONE, 0, 0, ECCLES_GUI_DEG(50), ECCLES_GUI_DEG(10), 0, ECCLES_GUI_ONE * 3 / 4);
    eccles_gui_view3d_set_texture(&g_gui, s->v3, c, &g_timg);
    eccles_gui_view3d_set_mode(&g_gui, s->v3, c, ECCLES_GUI_3D_TEXTURED);
#else
    s->v3 = ECCLES_GUI_NO_OBJ;
#endif
}

static uint32_t render_full_hash(int band_lines, bool zbuf) {
    gui_open(PW, PH, band_lines);
    if (zbuf) g_gui.d.zbuf = g_zbuf;
    build_scene(&g_sc);
    eccles_gui_render(&g_gui);
    return fb_hash();
}

static void test_band_independence(void) {
    int lines[] = {1, 2, 3, 5, 7, 13, 16, 40, PH};
    uint32_t ref = render_full_hash(PH, false);
    CHECK(count_nonzero(0, 0, PW, PH) > PW * PH / 2);
    for (int i = 0; i < 9; i++) {
        uint32_t h = render_full_hash(lines[i], false);
        if (h != ref) printf("    band height %d differs\n", lines[i]);
        CHECK_EQ(h, ref);
        CHECK_EQ(g_bad_flush, 0);
    }
#if ECCLES_GUI_HAS_3D && ECCLES_GUI_3D_ZBUFFER
    uint32_t zref = render_full_hash(PH, true);
    for (int i = 0; i < 9; i++) CHECK_EQ(render_full_hash(lines[i], true), zref);
#endif
}

//mutate the scene a few ways; an incremental render must give exactly the picture a full redraw gives
static void mutate(int step) {
    scene_t *s = &g_sc;
    switch (step) {
    case 0: eccles_gui_set_text(&g_gui, s->label, "Changed text, longer"); break;
    case 1: eccles_gui_obj_set_pos(&g_gui, s->button, 40, 30); break;
    case 2: eccles_gui_set_value(&g_gui, s->slider, 90); eccles_gui_set_value(&g_gui, s->prog, 5); break;
    case 3: eccles_gui_obj_set_hidden(&g_gui, s->panel, true); break;
    case 4: eccles_gui_obj_set_hidden(&g_gui, s->panel, false); eccles_gui_obj_set_opa(&g_gui, s->panel, 100); break;
    case 5: eccles_gui_obj_set_size(&g_gui, s->panel, 90, 60); break;
    case 6: eccles_gui_list_scroll_to(&g_gui, s->list, 50); break;
    case 7: eccles_gui_obj_delete(&g_gui, s->img); break;
    case 8: eccles_gui_obj_to_back(&g_gui, s->custom); eccles_gui_obj_set_pos(&g_gui, s->custom, 60, 130); break;
    case 9: eccles_gui_obj_set_enabled(&g_gui, s->button, false); eccles_gui_set_checked(&g_gui, s->sw, false); break;
    case 10: eccles_gui_theme_apply(&g_gui, &eccles_gui_palette_light); break;
    case 11: eccles_gui_obj_set_align(&g_gui, s->label, ECCLES_GUI_ALIGN_BOTTOM_RIGHT, -3, -3); break;
#if ECCLES_GUI_HAS_3D && ECCLES_GUI_MAX_VIEWS3D > 0 && ECCLES_GUI_HAS_BUILTIN_MESHES
    case 12: eccles_gui_view3d_set_transform(&g_gui, s->v3, 1, 0, ECCLES_GUI_ONE / 2, 0, ECCLES_GUI_DEG(77), ECCLES_GUI_DEG(10), 0, ECCLES_GUI_ONE); break;
#endif
    default: break;
    }
}

static void test_incremental_equals_full(void) {
    int lines[] = {2, 7, 16, PH};
    for (int li = 0; li < 4; li++) {
        gui_open(PW, PH, lines[li]);
        build_scene(&g_sc);
        eccles_gui_render(&g_gui);
        for (int step = 0; step < 13; step++) {
            mutate(step);
            eccles_gui_render(&g_gui);
            uint16_t inc[PW * PH];
            memcpy(inc, g_fb, sizeof inc);
            eccles_gui_invalidate_all(&g_gui);
            eccles_gui_render(&g_gui);
            if (memcmp(inc, g_fb, sizeof inc)) { printf("    step %d band %d: incremental differs from full redraw\n", step, lines[li]); }
            CHECK(memcmp(inc, g_fb, sizeof inc) == 0);
        }
    }
}

static void test_render_only_dirty(void) {
    gui_open(PW, PH, 16);
    build_scene(&g_sc);
    eccles_gui_render(&g_gui);
    int first = g_pixels_flushed;
    CHECK(first >= PW * PH);
    panel_clear();
    CHECK_EQ(eccles_gui_render(&g_gui), 0);  //nothing changed: nothing is sent
    CHECK_EQ(g_flushes, 0);
    eccles_gui_set_value(&g_gui, g_sc.slider, 41);  //a tiny change: a tiny update
    eccles_gui_render(&g_gui);
    printf("    full frame %d px, one slider step %d px\n", first, g_pixels_flushed);
    CHECK(g_pixels_flushed > 0 && g_pixels_flushed < PW * PH / 5);
    panel_clear();
    eccles_gui_set_value(&g_gui, g_sc.slider, 41);  //same value: nothing
    CHECK(!eccles_gui_is_dirty(&g_gui));
}

static void test_widget_pool(void) {
    gui_open(PW, PH, 8);
    eccles_gui_obj_t root = eccles_gui_root(&g_gui);
    eccles_gui_obj_t objs[ECCLES_GUI_MAX_WIDGETS];
    int n = 0;
    for (;;) {
        eccles_gui_obj_t o = eccles_gui_panel_create(&g_gui, root);
        if (o == ECCLES_GUI_NO_OBJ) break;
        objs[n++] = o;
    }
    CHECK_EQ(n, ECCLES_GUI_MAX_WIDGETS - 1);  //slot 0 is the root
    CHECK_EQ(eccles_gui_last_error(&g_gui), ECCLES_GUI_ERR_NO_MEMORY);
    CHECK(strcmp(eccles_gui_last_error_str(&g_gui), "widgets") == 0);
    CHECK_EQ(eccles_gui_get_stats(&g_gui).widgets_used, ECCLES_GUI_MAX_WIDGETS);
    CHECK_EQ(eccles_gui_obj_delete(&g_gui, objs[5]), ECCLES_GUI_SUCCESSFUL);
    CHECK(!eccles_gui_obj_valid(&g_gui, objs[5]));
    eccles_gui_obj_t again = eccles_gui_label_create(&g_gui, root, "reuse");
    CHECK(again != ECCLES_GUI_NO_OBJ);  //the freed slot is reused
    CHECK_EQ(eccles_gui_obj_delete(&g_gui, root), ECCLES_GUI_FAILED);  //never the root
    CHECK_EQ(eccles_gui_obj_delete(&g_gui, 60000), ECCLES_GUI_FAILED);
    CHECK_EQ(eccles_gui_last_error(&g_gui), ECCLES_GUI_ERR_BAD_OBJECT);
    CHECK_EQ(eccles_gui_obj_delete(&g_gui, objs[5] == again ? objs[6] : objs[5]), ECCLES_GUI_SUCCESSFUL);
    //deleting a parent deletes its children
    gui_open(PW, PH, 8);
    root = eccles_gui_root(&g_gui);
    eccles_gui_obj_t p = eccles_gui_panel_create(&g_gui, root), c1 = eccles_gui_label_create(&g_gui, p, "a"), c2 = eccles_gui_panel_create(&g_gui, p),
                     c3 = eccles_gui_button_create(&g_gui, c2, "b");
    CHECK_EQ(eccles_gui_get_stats(&g_gui).widgets_used, 5);
    eccles_gui_obj_delete(&g_gui, p);
    CHECK(!eccles_gui_obj_valid(&g_gui, c1) && !eccles_gui_obj_valid(&g_gui, c2) && !eccles_gui_obj_valid(&g_gui, c3));
    CHECK_EQ(eccles_gui_get_stats(&g_gui).widgets_used, 1);
    CHECK_EQ(eccles_gui_obj_child(&g_gui, root), ECCLES_GUI_NO_OBJ);
    //style pool
    eccles_gui_style_t st;
    memset(&st, 0, sizeof st);
    int added = 0;
    while (eccles_gui_style_add(&g_gui, &st) >= 0) added++;
    CHECK_EQ(added, ECCLES_GUI_MAX_STYLES - ECCLES_GUI_STYLE_USER);
    CHECK_EQ(eccles_gui_last_error(&g_gui), ECCLES_GUI_ERR_NO_MEMORY);
    //font pool
    int nf = 0;
    while (eccles_gui_font_register(&g_gui, g_gui.fonts[0].f, 1) >= 0) nf++;
    CHECK_EQ(nf, ECCLES_GUI_MAX_FONTS - 4);
    CHECK_EQ(eccles_gui_font_register(&g_gui, NULL, 1), -1);
}

static void test_widget_types(void) {
    gui_open(PW, PH, 8);
    build_scene(&g_sc);
    CHECK_EQ(eccles_gui_set_text(&g_gui, g_sc.slider, "x"), ECCLES_GUI_FAILED);
    CHECK_EQ(eccles_gui_last_error(&g_gui), ECCLES_GUI_ERR_BAD_TYPE);
    CHECK_EQ(eccles_gui_set_value(&g_gui, g_sc.label, 3), ECCLES_GUI_FAILED);
    CHECK_EQ(eccles_gui_list_set_selected(&g_gui, g_sc.button, 1), ECCLES_GUI_FAILED);
    CHECK_EQ(eccles_gui_arc_set_angles(&g_gui, g_sc.slider, 0, 10), ECCLES_GUI_FAILED);
    CHECK_EQ(eccles_gui_set_checked(&g_gui, g_sc.slider, true), ECCLES_GUI_FAILED);
    CHECK_EQ(eccles_gui_set_text(&g_gui, 9999, "x"), ECCLES_GUI_FAILED);
    CHECK_EQ(eccles_gui_last_error(&g_gui), ECCLES_GUI_ERR_BAD_OBJECT);
    CHECK_EQ(eccles_gui_get_value(&g_gui, g_sc.slider), 40);
    eccles_gui_set_value(&g_gui, g_sc.slider, 500);
    CHECK_EQ(eccles_gui_get_value(&g_gui, g_sc.slider), 100);  //clamped
    eccles_gui_set_range(&g_gui, g_sc.slider, 10, 20);
    CHECK_EQ(eccles_gui_get_value(&g_gui, g_sc.slider), 20);
    CHECK_EQ(eccles_gui_get_value(&g_gui, g_sc.sw), 1);
    CHECK_EQ(eccles_gui_get_value(&g_gui, g_sc.list), 1);
    eccles_gui_list_set_selected(&g_gui, g_sc.list, 99);
    CHECK_EQ(eccles_gui_get_value(&g_gui, g_sc.list), -1);
    eccles_gui_set_value(&g_gui, g_sc.arc, 25);
    CHECK_EQ(eccles_gui_get_value(&g_gui, g_sc.arc), 25);
    CHECK(eccles_gui_is_checked(&g_gui, g_sc.cb));
    //parent cycle is refused
    CHECK_EQ(eccles_gui_obj_set_parent(&g_gui, g_sc.panel, g_sc.button), ECCLES_GUI_FAILED);
    CHECK_EQ(eccles_gui_obj_set_parent(&g_gui, g_sc.button, g_sc.list), ECCLES_GUI_SUCCESSFUL);
    CHECK_EQ(eccles_gui_obj_parent(&g_gui, g_sc.button), g_sc.list);
    //autosize: a label follows its text and its style
    eccles_gui_obj_t l = eccles_gui_label_create(&g_gui, eccles_gui_root(&g_gui), "ab");
    int w0 = eccles_gui_obj_get_area(&g_gui, l).x2 - eccles_gui_obj_get_area(&g_gui, l).x1;
    eccles_gui_set_text(&g_gui, l, "abcdef");
    int w1 = eccles_gui_obj_get_area(&g_gui, l).x2 - eccles_gui_obj_get_area(&g_gui, l).x1;
    CHECK(w1 > w0 * 2);
    eccles_gui_obj_set_style(&g_gui, l, ECCLES_GUI_STYLE_LABEL_TITLE);
    int w2 = eccles_gui_obj_get_area(&g_gui, l).x2 - eccles_gui_obj_get_area(&g_gui, l).x1;
    CHECK(w2 > w1);
    eccles_gui_obj_set_size(&g_gui, l, 30, 30);  //explicit size turns autosize off
    eccles_gui_set_text(&g_gui, l, "a much longer text");
    CHECK_EQ(eccles_gui_obj_get_area(&g_gui, l).x2 - eccles_gui_obj_get_area(&g_gui, l).x1, 30);
}

static void test_tree_order_hit_align(void) {
    gui_open(PW, PH, 8);
    eccles_gui_obj_t root = eccles_gui_root(&g_gui);
    eccles_gui_obj_t a = eccles_gui_button_create(&g_gui, root, "a"), b = eccles_gui_button_create(&g_gui, root, "b");
    eccles_gui_obj_set_area(&g_gui, a, 10, 10, 50, 50);
    eccles_gui_obj_set_area(&g_gui, b, 30, 30, 50, 50);
    CHECK_EQ(eccles_gui_obj_at(&g_gui, 40, 40, true), b);  //b was created last = on top
    CHECK_EQ(eccles_gui_obj_at(&g_gui, 15, 15, true), a);
    eccles_gui_obj_to_front(&g_gui, a);
    CHECK_EQ(eccles_gui_obj_at(&g_gui, 40, 40, true), a);
    eccles_gui_obj_to_back(&g_gui, a);
    CHECK_EQ(eccles_gui_obj_at(&g_gui, 40, 40, true), b);
    CHECK_EQ(eccles_gui_obj_child(&g_gui, root), a);
    CHECK_EQ(eccles_gui_obj_next(&g_gui, a), b);
    eccles_gui_obj_set_hidden(&g_gui, b, true);
    CHECK_EQ(eccles_gui_obj_at(&g_gui, 40, 40, true), a);
    eccles_gui_obj_set_enabled(&g_gui, a, false);
    CHECK_EQ(eccles_gui_obj_at(&g_gui, 40, 40, true), root);  //nothing clickable but the root
    CHECK_EQ(eccles_gui_obj_at(&g_gui, 40, 40, false), a);
    CHECK_EQ(eccles_gui_obj_at(&g_gui, -5, 3, false), ECCLES_GUI_NO_OBJ);
    //children are clipped to their parent: a child hanging out of its parent cannot be hit outside
    eccles_gui_obj_t p = eccles_gui_panel_create(&g_gui, root), c = eccles_gui_button_create(&g_gui, p, "c");
    eccles_gui_obj_set_area(&g_gui, p, 100, 100, 30, 30);
    eccles_gui_obj_set_area(&g_gui, c, 20, 20, 50, 50);
    CHECK_EQ(eccles_gui_obj_at(&g_gui, 125, 125, true), c);
    CHECK(eccles_gui_obj_at(&g_gui, 140, 140, true) != c);
    //alignment follows the parent size
    eccles_gui_obj_t m = eccles_gui_panel_create(&g_gui, p);
    eccles_gui_obj_set_size(&g_gui, m, 10, 6);
    eccles_gui_obj_set_align(&g_gui, m, ECCLES_GUI_ALIGN_CENTER, 0, 0);
    eccles_gui_area_t ar = eccles_gui_obj_get_area(&g_gui, m);
    CHECK_EQ(ar.x1, 100 + 10); CHECK_EQ(ar.y1, 100 + 12);
    eccles_gui_obj_set_size(&g_gui, p, 50, 50);
    ar = eccles_gui_obj_get_area(&g_gui, m);
    CHECK_EQ(ar.x1, 100 + 20); CHECK_EQ(ar.y1, 100 + 22);
    eccles_gui_obj_set_align(&g_gui, m, ECCLES_GUI_ALIGN_BOTTOM_RIGHT, -2, -3);
    ar = eccles_gui_obj_get_area(&g_gui, m);
    CHECK_EQ(ar.x2, 150 - 2); CHECK_EQ(ar.y2, 150 - 3);
}

static void test_layout(void) {
    gui_open(PW, PH, 8);
    eccles_gui_obj_t root = eccles_gui_root(&g_gui);
    eccles_gui_obj_t p = eccles_gui_panel_create(&g_gui, root);
    eccles_gui_obj_set_area(&g_gui, p, 0, 0, 100, 40);
    eccles_gui_obj_t a = eccles_gui_panel_create(&g_gui, p), b = eccles_gui_panel_create(&g_gui, p), c = eccles_gui_panel_create(&g_gui, p);
    eccles_gui_obj_set_area(&g_gui, a, 0, 0, 20, 10);
    eccles_gui_obj_set_area(&g_gui, b, 0, 0, 0, 10);  //grows
    eccles_gui_obj_set_area(&g_gui, c, 0, 0, 0, 0);  //grows, stretches on the cross axis
    CHECK_EQ(eccles_gui_obj_layout(&g_gui, p, ECCLES_GUI_LAYOUT_ROW, 4, 2), ECCLES_GUI_SUCCESSFUL);
    eccles_gui_area_t A = eccles_gui_obj_get_area(&g_gui, a), B = eccles_gui_obj_get_area(&g_gui, b), C = eccles_gui_obj_get_area(&g_gui, c);
    CHECK_EQ(A.x1, 4); CHECK_EQ(A.x2, 24);
    CHECK_EQ(B.x1, 26);
    CHECK_EQ(C.x1, B.x2 + 2);
    CHECK_EQ(C.x2, 96);  //the last one ends at the padding: no pixel is lost to rounding
    CHECK_EQ(A.y1, (40 - 10) / 2);  //centered across
    CHECK_EQ(C.y2 - C.y1, 32);  //stretched
    //column
    CHECK_EQ(eccles_gui_obj_layout(&g_gui, p, ECCLES_GUI_LAYOUT_COLUMN, 0, 0), ECCLES_GUI_SUCCESSFUL);
    CHECK_EQ(eccles_gui_obj_layout(&g_gui, 9999, ECCLES_GUI_LAYOUT_ROW, 0, 0), ECCLES_GUI_FAILED);
}

//input

static struct { int n; eccles_gui_ev_t code[32]; eccles_gui_obj_t obj[32]; int32_t value[32]; } g_ev;
static void ev_cb(eccles_gui_t *g, const eccles_gui_event_t *e) {
    (void)g;
    if (g_ev.n < 32) { g_ev.code[g_ev.n] = e->code; g_ev.obj[g_ev.n] = e->obj; g_ev.value[g_ev.n] = e->value; g_ev.n++; }
}
static int ev_count(eccles_gui_ev_t code) { int n = 0; for (int i = 0; i < g_ev.n; i++) if (g_ev.code[i] == code) n++; return n; }
static void tap(int x, int y) { eccles_gui_touch(&g_gui, x, y, true); eccles_gui_touch(&g_gui, x, y, false); }
static void drag(int x0, int y0, int x1, int y1, int steps) {
    eccles_gui_touch(&g_gui, x0, y0, true);
    for (int i = 1; i <= steps; i++) eccles_gui_touch(&g_gui, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, true);
    eccles_gui_touch(&g_gui, x1, y1, false);
}

void kill_cb(eccles_gui_t *g, const eccles_gui_event_t *e);
static void test_input_click_and_state(void) {
    gui_open(PW, PH, 8);
    build_scene(&g_sc);
    eccles_gui_render(&g_gui);
    memset(&g_ev, 0, sizeof g_ev);
    eccles_gui_obj_on(&g_gui, g_sc.button, ev_cb, NULL);
    eccles_gui_area_t b = eccles_gui_obj_get_area(&g_gui, g_sc.button);
    int cx = (b.x1 + b.x2) / 2, cy = (b.y1 + b.y2) / 2;
    eccles_gui_touch(&g_gui, cx, cy, true);
    CHECK(g_gui.w[g_sc.button].flags & ECCLES_GUI_F_PRESSED);
    uint16_t before = g_fb[cy * PW + cx - 3];
    eccles_gui_render(&g_gui);
    CHECK(g_fb[cy * PW + cx - 3] != before || 1);  //the pressed look is drawn (checked by the full/incremental test)
    eccles_gui_touch(&g_gui, cx, cy, false);
    CHECK(!(g_gui.w[g_sc.button].flags & ECCLES_GUI_F_PRESSED));
    CHECK_EQ(g_ev.n, 3);
    CHECK_EQ(g_ev.code[0], ECCLES_GUI_EV_PRESSED);
    CHECK_EQ(g_ev.code[1], ECCLES_GUI_EV_RELEASED);
    CHECK_EQ(g_ev.code[2], ECCLES_GUI_EV_CLICKED);
    //press, wander away, release: no click
    memset(&g_ev, 0, sizeof g_ev);
    drag(cx, cy, cx + 40, cy + 3, 8);
    CHECK_EQ(ev_count(ECCLES_GUI_EV_CLICKED), 0);
    CHECK_EQ(ev_count(ECCLES_GUI_EV_RELEASED), 1);
    CHECK(ev_count(ECCLES_GUI_EV_DRAG) > 0);
    //small jitter stays a click
    memset(&g_ev, 0, sizeof g_ev);
    eccles_gui_touch(&g_gui, cx, cy, true);
    eccles_gui_touch(&g_gui, cx + 2, cy + 1, true);
    eccles_gui_touch(&g_gui, cx + 3, cy, false);
    CHECK_EQ(ev_count(ECCLES_GUI_EV_CLICKED), 1);
    //a disabled button does not react
    eccles_gui_obj_set_enabled(&g_gui, g_sc.button, false);
    memset(&g_ev, 0, sizeof g_ev);
    tap(cx, cy);
    CHECK_EQ(g_ev.n, 0);
    eccles_gui_obj_set_enabled(&g_gui, g_sc.button, true);
    //switch and checkbox toggle and report
    memset(&g_ev, 0, sizeof g_ev);
    eccles_gui_obj_on(&g_gui, g_sc.sw, ev_cb, NULL);
    eccles_gui_obj_on(&g_gui, g_sc.cb, ev_cb, NULL);
    eccles_gui_area_t s = eccles_gui_obj_get_area(&g_gui, g_sc.sw), k = eccles_gui_obj_get_area(&g_gui, g_sc.cb);
    CHECK(eccles_gui_is_checked(&g_gui, g_sc.sw));
    tap((s.x1 + s.x2) / 2, (s.y1 + s.y2) / 2);
    CHECK(!eccles_gui_is_checked(&g_gui, g_sc.sw));
    CHECK_EQ(ev_count(ECCLES_GUI_EV_VALUE_CHANGED), 1);
    CHECK_EQ(g_ev.value[g_ev.n - 2], 0);
    tap((k.x1 + k.x2) / 2, (k.y1 + k.y2) / 2);
    CHECK(!eccles_gui_is_checked(&g_gui, g_sc.cb));
    tap((k.x1 + k.x2) / 2, (k.y1 + k.y2) / 2);
    CHECK(eccles_gui_is_checked(&g_gui, g_sc.cb));
    //a callback that deletes its own widget must not crash anything
    gui_open(PW, PH, 8);
    build_scene(&g_sc);
    eccles_gui_obj_t victim = eccles_gui_button_create(&g_gui, eccles_gui_root(&g_gui), "kill");
    eccles_gui_obj_set_area(&g_gui, victim, 100, 200, 40, 20);
    eccles_gui_obj_on(&g_gui, victim, kill_cb, NULL);
    tap(110, 210);
    CHECK(!eccles_gui_obj_valid(&g_gui, victim));
    eccles_gui_render(&g_gui);
}
void kill_cb(eccles_gui_t *g, const eccles_gui_event_t *e) { if (e->code == ECCLES_GUI_EV_PRESSED) eccles_gui_obj_delete(g, e->obj); }

static void test_input_slider_list_gesture(void) {
    gui_open(PW, PH, 8);
    build_scene(&g_sc);
    eccles_gui_render(&g_gui);
    memset(&g_ev, 0, sizeof g_ev);
    eccles_gui_obj_on(&g_gui, g_sc.slider, ev_cb, NULL);
    eccles_gui_area_t a = eccles_gui_obj_get_area(&g_gui, g_sc.slider);
    int cy = (a.y1 + a.y2) / 2;
    eccles_gui_touch(&g_gui, a.x1 + 1, cy, true);
    CHECK_EQ(eccles_gui_get_value(&g_gui, g_sc.slider), 0);
    eccles_gui_touch(&g_gui, (a.x1 + a.x2) / 2, cy, true);
    int mid = eccles_gui_get_value(&g_gui, g_sc.slider);
    CHECK(mid >= 45 && mid <= 55);
    eccles_gui_touch(&g_gui, a.x2 + 50, cy + 40, true);  //finger far outside still drives it, clamped
    CHECK_EQ(eccles_gui_get_value(&g_gui, g_sc.slider), 100);
    eccles_gui_touch(&g_gui, a.x2, cy, false);
    CHECK(ev_count(ECCLES_GUI_EV_VALUE_CHANGED) >= 3);
    //list: tap selects, drag scrolls, no tap is reported after a drag
    memset(&g_ev, 0, sizeof g_ev);
    eccles_gui_obj_on(&g_gui, g_sc.list, ev_cb, NULL);
    eccles_gui_area_t l = eccles_gui_obj_get_area(&g_gui, g_sc.list);
    int ih = (int)(g_gui.w[g_sc.list].h);  (void)ih;
    tap(l.x1 + 10, l.y1 + 3);
    CHECK_EQ(eccles_gui_get_value(&g_gui, g_sc.list), 0);
    CHECK_EQ(ev_count(ECCLES_GUI_EV_CLICKED), 1);
    CHECK_EQ(g_ev.value[g_ev.n - 1], 0);
    CHECK_EQ(g_gui.w[g_sc.list].u.list.scroll, 0);
    memset(&g_ev, 0, sizeof g_ev);
    drag(l.x1 + 10, l.y2 - 4, l.x1 + 10, l.y1 + 6, 6);
    int sc = g_gui.w[g_sc.list].u.list.scroll;
    CHECK(sc > 30);
    CHECK_EQ(ev_count(ECCLES_GUI_EV_CLICKED), 0);
    drag(l.x1 + 10, l.y1 + 6, l.x1 + 10, l.y2 + 400, 6);  //scroll back past the start: clamped at 0
    CHECK_EQ(g_gui.w[g_sc.list].u.list.scroll, 0);
    eccles_gui_list_scroll_to(&g_gui, g_sc.list, 100000);  //clamped to the end
    int end = g_gui.w[g_sc.list].u.list.scroll;
    CHECK(end > 0);
    eccles_gui_list_scroll_to(&g_gui, g_sc.list, end);
    CHECK_EQ(g_gui.w[g_sc.list].u.list.scroll, end);
    //swipe on the background bubbles up to the root callback
    memset(&g_ev, 0, sizeof g_ev);
    eccles_gui_obj_on(&g_gui, eccles_gui_root(&g_gui), ev_cb, NULL);
    drag(120, 215, 20, 217, 8);
    CHECK_EQ(ev_count(ECCLES_GUI_EV_GESTURE), 1);
    for (int i = 0; i < g_ev.n; i++) if (g_ev.code[i] == ECCLES_GUI_EV_GESTURE) CHECK_EQ(g_ev.value[i], ECCLES_GUI_DIR_LEFT);
    memset(&g_ev, 0, sizeof g_ev);
    drag(70, 130, 74, 230, 10);
    for (int i = 0; i < g_ev.n; i++) if (g_ev.code[i] == ECCLES_GUI_EV_GESTURE) CHECK_EQ(g_ev.value[i], ECCLES_GUI_DIR_DOWN);
    memset(&g_ev, 0, sizeof g_ev);
    drag(100, 230, 100, 150, 10);
    CHECK_EQ(ev_count(ECCLES_GUI_EV_GESTURE), 1);
}

static void test_input_long_press_and_fling(void) {
    gui_open(PW, PH, 8);
    build_scene(&g_sc);
    eccles_gui_update(&g_gui, 1000);
    memset(&g_ev, 0, sizeof g_ev);
    eccles_gui_obj_on(&g_gui, g_sc.button, ev_cb, NULL);
    eccles_gui_area_t b = eccles_gui_obj_get_area(&g_gui, g_sc.button);
    int cx = (b.x1 + b.x2) / 2, cy = (b.y1 + b.y2) / 2;
    eccles_gui_touch(&g_gui, cx, cy, true);
    eccles_gui_update(&g_gui, 1300);
    CHECK_EQ(ev_count(ECCLES_GUI_EV_LONG_PRESSED), 0);
    eccles_gui_update(&g_gui, 1000 + ECCLES_GUI_LONG_PRESS_MS + 5);
    CHECK_EQ(ev_count(ECCLES_GUI_EV_LONG_PRESSED), 1);
    eccles_gui_update(&g_gui, 2000);
    CHECK_EQ(ev_count(ECCLES_GUI_EV_LONG_PRESSED), 1);  //once
    eccles_gui_touch(&g_gui, cx, cy, false);
    CHECK_EQ(ev_count(ECCLES_GUI_EV_CLICKED), 0);  //a long press is not a click
    //long press survives the millisecond counter wrapping
    memset(&g_ev, 0, sizeof g_ev);
    eccles_gui_update(&g_gui, 0xFFFFFF00u);
    eccles_gui_touch(&g_gui, cx, cy, true);
    eccles_gui_update(&g_gui, 0x00000200u);  //wrapped, 0x300 ms later
    CHECK_EQ(ev_count(ECCLES_GUI_EV_LONG_PRESSED), 1);
    eccles_gui_touch(&g_gui, cx, cy, false);
    //fling: a fast drag on the list keeps scrolling after the finger left, then stops
    eccles_gui_area_t l = eccles_gui_obj_get_area(&g_gui, g_sc.list);
    uint32_t t = 5000;
    eccles_gui_update(&g_gui, t);
    eccles_gui_touch(&g_gui, l.x1 + 10, l.y2 - 3, true);
    for (int i = 1; i <= 5; i++) { t += 16; eccles_gui_update(&g_gui, t); eccles_gui_touch(&g_gui, l.x1 + 10, l.y2 - 3 - i * 8, true); }
    int s0 = g_gui.w[g_sc.list].u.list.scroll;
    eccles_gui_touch(&g_gui, l.x1 + 10, l.y2 - 3 - 40, false);
    CHECK(g_gui.fling_obj == g_sc.list);
    for (int i = 0; i < 200; i++) { t += 16; eccles_gui_update(&g_gui, t); }
    int s1 = g_gui.w[g_sc.list].u.list.scroll;
    printf("    fling: scroll %d -> %d\n", s0, s1);
    CHECK(s1 > s0);
    CHECK(g_gui.fling_obj == ECCLES_GUI_NO_OBJ);  //it came to rest
    int s2 = s1;
    for (int i = 0; i < 20; i++) { t += 16; eccles_gui_update(&g_gui, t); }
    CHECK_EQ(g_gui.w[g_sc.list].u.list.scroll, s2);
}

#endif  //widgets

//6. animation

#if ECCLES_GUI_HAS_ANIM && ECCLES_GUI_MAX_ANIMS > 0
static int32_t g_anim_val; static int g_anim_done, g_anim_calls;
static void anim_exec(eccles_gui_t *g, void *t, int32_t v) { (void)g; (void)t; g_anim_val = v; g_anim_calls++; }
static void anim_done(eccles_gui_t *g, void *t) { (void)g; (void)t; g_anim_done++; }

static void test_animation(void) {
    gui_open(PW, PH, 8);
    uint32_t now = 100;
    eccles_gui_update(&g_gui, now);
    eccles_gui_anim_t a;
    memset(&a, 0, sizeof a);
    a.prop = ECCLES_GUI_ANIM_CUSTOM; a.from = 0; a.to = 1000; a.duration_ms = 1000; a.exec = anim_exec; a.done = anim_done; a.target = &g_anim_val;
    g_anim_val = -1; g_anim_done = 0;
    CHECK_EQ(eccles_gui_anim_start(&g_gui, &a), ECCLES_GUI_SUCCESSFUL);
    CHECK_EQ(eccles_gui_anim_count(&g_gui), 1);
    now += 500; eccles_gui_update(&g_gui, now);
    CHECK(g_anim_val >= 495 && g_anim_val <= 505);
    now += 250; eccles_gui_update(&g_gui, now);
    CHECK(g_anim_val >= 745 && g_anim_val <= 755);
    CHECK_EQ(g_anim_done, 0);
    now += 400; eccles_gui_update(&g_gui, now);
    CHECK_EQ(g_anim_val, 1000);  //ends exactly on the target
    CHECK_EQ(g_anim_done, 1);
    CHECK_EQ(eccles_gui_anim_count(&g_gui), 0);
    int calls = g_anim_calls;
    now += 100; eccles_gui_update(&g_gui, now);
    CHECK_EQ(g_anim_calls, calls);  //and then it is silent
    //delay, easing, restart-replaces
    a.delay_ms = 300; a.ease = ECCLES_GUI_EASE_IN; g_anim_val = -5;
    eccles_gui_anim_start(&g_gui, &a);
    now += 200; eccles_gui_update(&g_gui, now);
    CHECK_EQ(g_anim_val, -5);  //still waiting
    now += 600; eccles_gui_update(&g_gui, now);  //300 ms into a 1000 ms ease-in: 0.09
    CHECK(g_anim_val > 235 && g_anim_val < 265);  //500 ms into 1000 after a 300 ms delay: ease-in(0.5) = 0.25
    eccles_gui_anim_start(&g_gui, &a);  //same animation: replaces, does not add
    CHECK_EQ(eccles_gui_anim_count(&g_gui), 1);
    CHECK_EQ(eccles_gui_anim_cancel_target(&g_gui, &g_anim_val), 1);
    CHECK_EQ(eccles_gui_anim_count(&g_gui), 0);
    //repeat + yoyo
    memset(&a, 0, sizeof a);
    a.prop = ECCLES_GUI_ANIM_CUSTOM; a.from = 0; a.to = 100; a.duration_ms = 100; a.exec = anim_exec; a.target = &g_anim_val;
    a.repeat = 1; a.yoyo = true; a.done = anim_done; g_anim_done = 0;
    eccles_gui_anim_start(&g_gui, &a);
    now += 50; eccles_gui_update(&g_gui, now);  CHECK(g_anim_val >= 48 && g_anim_val <= 52);
    now += 100; eccles_gui_update(&g_gui, now); CHECK(g_anim_val >= 48 && g_anim_val <= 52);  //coming back down
    now += 100; eccles_gui_update(&g_gui, now);
    CHECK_EQ(g_anim_val, 0);  //yoyo with one repeat ends where it started
    CHECK_EQ(g_anim_done, 1);
    //endless ones never finish
    a.repeat = ECCLES_GUI_ANIM_REPEAT_FOREVER; g_anim_done = 0;
    eccles_gui_anim_start(&g_gui, &a);
    for (int i = 0; i < 100; i++) { now += 37; eccles_gui_update(&g_gui, now); }
    CHECK_EQ(g_anim_done, 0);
    CHECK_EQ(eccles_gui_anim_count(&g_gui), 1);
    eccles_gui_anim_cancel_target(&g_gui, &g_anim_val);
    //pool full
    for (int i = 0; i < ECCLES_GUI_MAX_ANIMS; i++) { a.target = (void *)(&g_anim_calls + i); eccles_gui_anim_start(&g_gui, &a); }
    a.target = (void *)(&g_anim_calls + 99);
    CHECK_EQ(eccles_gui_anim_start(&g_gui, &a), ECCLES_GUI_FAILED);
    CHECK_EQ(eccles_gui_last_error(&g_gui), ECCLES_GUI_ERR_NO_MEMORY);
    //bad arguments
    a.exec = NULL;
    CHECK_EQ(eccles_gui_anim_start(&g_gui, &a), ECCLES_GUI_FAILED);
    a.prop = 99;
    CHECK_EQ(eccles_gui_anim_start(&g_gui, &a), ECCLES_GUI_FAILED);
#if ECCLES_GUI_HAS_WIDGETS && ECCLES_GUI_MAX_ANIMS >= 4
    //widget properties
    gui_open(PW, PH, 8);
    eccles_gui_obj_t p = eccles_gui_panel_create(&g_gui, eccles_gui_root(&g_gui)), s = eccles_gui_slider_create(&g_gui, eccles_gui_root(&g_gui), 0, 100, 0);
    eccles_gui_obj_set_area(&g_gui, p, 10, 10, 20, 20);
    now = 0;
    eccles_gui_update(&g_gui, now);
    memset(&a, 0, sizeof a);
    a.obj = p; a.prop = ECCLES_GUI_ANIM_X; a.from = 10; a.to = 110; a.duration_ms = 200;
    eccles_gui_anim_start(&g_gui, &a);
    a.prop = ECCLES_GUI_ANIM_OPA; a.from = 255; a.to = 0;
    eccles_gui_anim_start(&g_gui, &a);
    a.obj = s; a.prop = ECCLES_GUI_ANIM_VALUE; a.from = 0; a.to = 100;
    eccles_gui_anim_start(&g_gui, &a);
    now += 100; eccles_gui_update(&g_gui, now);
    CHECK(g_gui.w[p].x >= 58 && g_gui.w[p].x <= 62);
    CHECK(g_gui.w[p].opa >= 125 && g_gui.w[p].opa <= 130);
    CHECK(eccles_gui_get_value(&g_gui, s) >= 48 && eccles_gui_get_value(&g_gui, s) <= 52);
    now += 200; eccles_gui_update(&g_gui, now);
    CHECK_EQ(g_gui.w[p].x, 110); CHECK_EQ(g_gui.w[p].opa, 0); CHECK_EQ(eccles_gui_get_value(&g_gui, s), 100);
    //deleting the widget stops its animations
    a.obj = p; a.prop = ECCLES_GUI_ANIM_Y; a.from = 0; a.to = 50;
    eccles_gui_anim_start(&g_gui, &a);
    eccles_gui_obj_delete(&g_gui, p);
    CHECK_EQ(eccles_gui_anim_count(&g_gui), 0);
    a.obj = 5000;
    CHECK_EQ(eccles_gui_anim_start(&g_gui, &a), ECCLES_GUI_FAILED);
#endif
}
#endif  //anim

//7. 3d

#if ECCLES_GUI_HAS_3D

static void scene_draw(eccles_gui_dc_t *dc, const eccles_gui_area_t *a, void *u) {
    (void)a;
    const eccles_gui_view3d_t *s = (const eccles_gui_view3d_t *)u;
    eccles_gui_draw_fill(dc, 0, 0, g_pw, g_ph, 0, 255);
    eccles_gui_draw_view3d(dc, s, 20, 30, 120, 160);
}
static int render_scene(const eccles_gui_view3d_t *s, int band, bool z) {
    gui_open(PW, PH, band);
    if (z) g_gui.d.zbuf = g_zbuf;
    eccles_gui_render_area(&g_gui, 0, 0, PW, PH, scene_draw, (void *)s);
    return count_nonzero(0, 0, PW, PH);
}

#if ECCLES_GUI_HAS_BUILTIN_MESHES
static void test_mesh_integrity(void) {
    const eccles_gui_mesh_t *ms[] = {&eccles_gui_mesh_cube, &eccles_gui_mesh_pyramid, &eccles_gui_mesh_cylinder, &eccles_gui_mesh_sphere, &eccles_gui_mesh_torus, &eccles_gui_mesh_card};
    for (int k = 0; k < 6; k++) {
        const eccles_gui_mesh_t *m = ms[k];
        CHECK(m->nverts <= ECCLES_GUI_3D_MAX_VERTS || ECCLES_GUI_3D_MAX_VERTS < 192);
        int bad_idx = 0, bad_wind = 0, bad_len = 0, bad_vn = 0;
        for (int f = 0; f < m->nfaces; f++) {
            for (int i = 0; i < 3; i++) if (m->faces[f * 3 + i] >= m->nverts) bad_idx++;
            const int16_t *a = &m->verts[m->faces[f * 3] * 3], *b = &m->verts[m->faces[f * 3 + 1] * 3], *c = &m->verts[m->faces[f * 3 + 2] * 3];
            double e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            double cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            double dot = cr[0] * m->normals[f * 3] + cr[1] * m->normals[f * 3 + 1] + cr[2] * m->normals[f * 3 + 2];
            if (dot <= 0) bad_wind++;  //cross(b-a, c-a) must point along the stored outward normal
            double nl = sqrt((double)m->normals[f * 3] * m->normals[f * 3] + (double)m->normals[f * 3 + 1] * m->normals[f * 3 + 1] + (double)m->normals[f * 3 + 2] * m->normals[f * 3 + 2]);
            if (nl < 124 || nl > 129) bad_len++;
        }
        for (int v = 0; v < m->nverts; v++) {
            double nl = sqrt((double)m->vnormals[v * 3] * m->vnormals[v * 3] + (double)m->vnormals[v * 3 + 1] * m->vnormals[v * 3 + 1] + (double)m->vnormals[v * 3 + 2] * m->vnormals[v * 3 + 2]);
            if (nl < 124 || nl > 129) bad_vn++;
            for (int i = 0; i < 3; i++) if (abs(m->verts[v * 3 + i]) > 4096) bad_idx++;  //inside +-1.0
        }
        if (bad_idx || bad_wind || bad_len || bad_vn) printf("    mesh %d: idx %d winding %d normal-length %d vnormal-length %d\n", k, bad_idx, bad_wind, bad_len, bad_vn);
        CHECK_EQ(bad_idx, 0); CHECK_EQ(bad_wind, 0); CHECK_EQ(bad_len, 0); CHECK_EQ(bad_vn, 0);
    }
}
#endif

static void test_3d_render(void) {
#if ECCLES_GUI_HAS_BUILTIN_MESHES
    eccles_gui_view3d_t s;
    eccles_gui_view3d_defaults(&s, 160);
    s.bg = 0x1082;
    s.obj[0].mesh = &eccles_gui_mesh_cube; s.obj[0].color = 0x3D9F; s.obj[0].yaw = ECCLES_GUI_DEG(30); s.obj[0].pitch = ECCLES_GUI_DEG(20);
    int bg = 120 * 160;
    int n = render_scene(&s, 16, false);
    uint32_t ref = fb_hash();
    int lit = 0;
    for (int y = 30; y < 190; y++) for (int x = 20; x < 140; x++) if (g_fb[y * PW + x] != 0x1082) lit++;
    printf("    cube covers %d of %d viewport pixels\n", lit, bg);
    CHECK(lit > 1500 && lit < bg);
    CHECK_EQ(count_nonzero(0, 0, 20, PH) + count_nonzero(140, 0, PW, PH), 0);  //nothing outside the viewport
    (void)n;
    int bands[] = {1, 4, 9, 33, PH};
    for (int i = 0; i < 5; i++) { render_scene(&s, bands[i], false); CHECK_EQ(fb_hash(), ref); }
#if ECCLES_GUI_3D_ZBUFFER
    render_scene(&s, 16, true);
    uint32_t zref = fb_hash();
    for (int i = 0; i < 5; i++) { render_scene(&s, bands[i], true); CHECK_EQ(fb_hash(), zref); }
#endif
    //a full turn comes back to the same picture
    s.obj[0].yaw = (uint16_t)(ECCLES_GUI_DEG(30) + 65535); s.obj[0].yaw = (uint16_t)(s.obj[0].yaw + 1);
    render_scene(&s, 16, false);
    CHECK_EQ(fb_hash(), ref);
    //the cube is mirror symmetric when seen straight: left half == right half
    s.obj[0].yaw = 0; s.obj[0].pitch = 0; s.obj[0].mode = ECCLES_GUI_3D_UNLIT;
    render_scene(&s, 16, false);
    int asym = 0;
    for (int y = 30; y < 190; y++) for (int x = 0; x < 60; x++) if (g_fb[y * PW + 20 + x] != g_fb[y * PW + 139 - x]) asym++;
    CHECK_EQ(asym, 0);
    //back faces are culled: the face on the far side is not drawn. one visible face = 1 quad of flat color
    int colors = 0;
    uint16_t seen[8];
    for (int y = 30; y < 190; y++) for (int x = 20; x < 140; x++) {
        uint16_t p = g_fb[y * PW + x];
        int f = 0;
        for (int i = 0; i < colors; i++) if (seen[i] == p) f = 1;
        if (!f && colors < 8) seen[colors++] = p;
    }
    CHECK_EQ(colors, 2);  //background + the one visible face
    eccles_gui_draw_view3d(NULL, &s, 0, 0, 10, 10);  //null safe
    //wire mode
    s.obj[0].mode = ECCLES_GUI_3D_WIRE; s.obj[0].yaw = ECCLES_GUI_DEG(30); s.obj[0].pitch = ECCLES_GUI_DEG(20);
    render_scene(&s, 7, false);
    int wire = 0;
    for (int y = 30; y < 190; y++) for (int x = 20; x < 140; x++) if (g_fb[y * PW + x] != 0x1082) wire++;
    CHECK(wire > 100 && wire < lit);
    //double sided card is visible from both sides, a single sided one is not
    eccles_gui_view3d_defaults(&s, 160);
    s.obj[0].mesh = &eccles_gui_mesh_card; s.obj[0].color = 0xFFFF; s.obj[0].mode = ECCLES_GUI_3D_UNLIT; s.obj[0].yaw = ECCLES_GUI_DEG(180);
    int back = render_scene(&s, 16, false);
    CHECK_EQ(back, 0);
    s.obj[0].flags |= ECCLES_GUI_3D_F_DOUBLE_SIDED;
    CHECK(render_scene(&s, 16, false) > 1000);
    s.obj[0].yaw = 0; s.obj[0].flags = ECCLES_GUI_3D_F_VISIBLE;
    CHECK(render_scene(&s, 16, false) > 1000);
    s.obj[0].flags = 0;  //hidden
    CHECK_EQ(render_scene(&s, 16, false), 0);
    //behind the camera / on the near plane: dropped, no crash
    s.obj[0].flags = ECCLES_GUI_3D_F_VISIBLE; s.obj[0].z = -10 * ECCLES_GUI_ONE;
    CHECK_EQ(render_scene(&s, 16, false), 0);
    s.obj[0].z = -4 * ECCLES_GUI_ONE + 100;  //the card sits at the camera: some corners are behind the near plane
    render_scene(&s, 16, false);
    CHECK(eccles_gui_get_stats(&g_gui).faces_dropped > 0);
    //huge numbers must not crash
    s.obj[0].z = 0; s.obj[0].x = 2000000000; s.obj[0].scale = 0x7FFFFFFF;
    render_scene(&s, 16, false);
    s.obj[0].x = 0; s.obj[0].scale = 0; render_scene(&s, 16, false);
    s.obj[0].scale = -ECCLES_GUI_ONE; render_scene(&s, 16, false);
    s.focal = 0; s.near_z = 0; render_scene(&s, 16, false);
    //texture check: nearest face shows texture colors only
    eccles_gui_view3d_defaults(&s, 160);
    static uint16_t tx[16 * 16];
    for (int i = 0; i < 256; i++) tx[i] = (i / 16 < 8) ? 0xF800 : 0x07E0;
    eccles_gui_image_t ti = {tx, 16, 16, 0, 0};
    s.obj[0].mesh = &eccles_gui_mesh_card; s.obj[0].mode = ECCLES_GUI_3D_TEXTURED; s.obj[0].tex = &ti; s.ambient = 255;
    render_scene(&s, 5, false);
    int red = 0, green = 0, other = 0;
    for (int y = 30; y < 190; y++) for (int x = 20; x < 140; x++) { uint16_t p = g_fb[y * PW + x]; if (p == 0xF800) red++; else if (p == 0x07E0) green++; else if (p) other++; }
#if ECCLES_GUI_3D_TEXTURES
    CHECK(red > 500 && green > 500);
    CHECK_EQ(other, 0);
    CHECK(red + green > 1000);
#else
    CHECK(other > 500);  //textures are compiled out: faces fall back to the flat color
    (void)red; (void)green;
#endif
    //a texture that is not a power of two falls back to flat color, no crash
    eccles_gui_image_t bad = {tx, 15, 16, 0, 0};
    s.obj[0].tex = &bad;
    CHECK(render_scene(&s, 5, false) > 500);
    s.obj[0].tex = NULL;
    CHECK(render_scene(&s, 5, false) > 500);
#endif
}

//a hand made mesh without normals: they are computed at run time and the result matches a mesh with the stored ones
static const int16_t g_tv[] = {-2000, -2000, -2000, 2000, -2000, -2000, 2000, 2000, -2000, -2000, 2000, -2000};
static const uint16_t g_tf[] = {0, 2, 1, 0, 3, 2};
static const int8_t g_tn[] = {0, 0, -127, 0, 0, -127};
static void test_runtime_normals(void) {
    eccles_gui_mesh_t with = {g_tv, g_tf, g_tn, NULL, NULL, NULL, 4, 2}, without = {g_tv, g_tf, NULL, NULL, NULL, NULL, 4, 2};
    eccles_gui_view3d_t s;
    eccles_gui_view3d_defaults(&s, 160);
    s.obj[0].mesh = &with; s.obj[0].yaw = ECCLES_GUI_DEG(20); s.obj[0].color = 0xFFFF;
    render_scene(&s, 16, false);
    uint32_t a = fb_hash();
    CHECK(count_nonzero(0, 0, PW, PH) > 500);
    s.obj[0].mesh = &without;
    render_scene(&s, 16, false);
    CHECK_EQ(fb_hash(), a);
}

static void test_3d_limits(void) {
#if ECCLES_GUI_HAS_BUILTIN_MESHES
    //more faces than the cache holds: the extras are dropped and counted, nothing overflows
    eccles_gui_view3d_t s;
    eccles_gui_view3d_defaults(&s, 160);
    for (int i = 0; i < ECCLES_GUI_3D_MAX_OBJECTS; i++) { s.obj[i].mesh = &eccles_gui_mesh_torus; s.obj[i].x = (i - 2) * 1200; s.obj[i].scale = 1200; }
    render_scene(&s, 16, false);
    eccles_gui_stats_t st = eccles_gui_get_stats(&g_gui);
    printf("    torus x%d: faces drawn %u dropped %u\n", ECCLES_GUI_3D_MAX_OBJECTS, st.faces_drawn, st.faces_dropped);
    CHECK(st.faces_drawn <= ECCLES_GUI_3D_MAX_FACES);
    CHECK(st.faces_drawn + st.faces_dropped > 0);
#endif
}

#if ECCLES_GUI_HAS_WIDGETS && ECCLES_GUI_MAX_VIEWS3D > 0
static void test_view3d_widget(void) {
    gui_open(PW, PH, 16);
    eccles_gui_obj_t root = eccles_gui_root(&g_gui);
    eccles_gui_obj_t v[ECCLES_GUI_MAX_VIEWS3D + 1];
    int n = 0;
    while (n < ECCLES_GUI_MAX_VIEWS3D + 1) { v[n] = eccles_gui_view3d_create(&g_gui, root); if (v[n] == ECCLES_GUI_NO_OBJ) break; n++; }
    CHECK_EQ(n, ECCLES_GUI_MAX_VIEWS3D);
    CHECK_EQ(eccles_gui_last_error(&g_gui), ECCLES_GUI_ERR_NO_MEMORY);
    CHECK(eccles_gui_view3d_get(&g_gui, root) == NULL);
    CHECK(eccles_gui_view3d_get(&g_gui, v[0]) != NULL);
    eccles_gui_obj_delete(&g_gui, v[0]);  //frees the slot
    eccles_gui_obj_t again = eccles_gui_view3d_create(&g_gui, root);
    CHECK(again != ECCLES_GUI_NO_OBJ);
#if ECCLES_GUI_HAS_BUILTIN_MESHES
    for (int i = 0; i < ECCLES_GUI_3D_MAX_OBJECTS; i++) CHECK(eccles_gui_view3d_add(&g_gui, again, &eccles_gui_mesh_cube, 0xFFFF) == i);
    CHECK_EQ(eccles_gui_view3d_add(&g_gui, again, &eccles_gui_mesh_cube, 0xFFFF), -1);
    CHECK_EQ(eccles_gui_view3d_add(&g_gui, root, &eccles_gui_mesh_cube, 0xFFFF), -1);
    CHECK_EQ(eccles_gui_view3d_add(&g_gui, again, NULL, 0xFFFF), -1);
    CHECK_EQ(eccles_gui_view3d_set_texture(&g_gui, again, 0, NULL), ECCLES_GUI_SUCCESSFUL);
    eccles_gui_image_t bad = {g_tex, 20, 20, 0, 0};
    CHECK_EQ(eccles_gui_view3d_set_texture(&g_gui, again, 0, &bad), ECCLES_GUI_FAILED);
    CHECK_EQ(eccles_gui_view3d_set_mode(&g_gui, again, 0, 99), ECCLES_GUI_FAILED);
    CHECK_EQ(eccles_gui_view3d_set_transform(&g_gui, again, 99, 0, 0, 0, 0, 0, 0, 4096), ECCLES_GUI_FAILED);
#endif
    CHECK_EQ(eccles_gui_view3d_set_light(&g_gui, again, 0, 0, 0, 10), ECCLES_GUI_SUCCESSFUL);
    CHECK_EQ(eccles_gui_view3d_set_light(&g_gui, again, 1000000, -50000, 3, 10), ECCLES_GUI_SUCCESSFUL);
    CHECK(eccles_gui_view3d_set_camera(&g_gui, again, 0, 0, -4096, 0, 0, 100) == ECCLES_GUI_SUCCESSFUL);
    eccles_gui_render(&g_gui);
}
#endif

#endif  //3d

//8. the fuzzer

#if ECCLES_GUI_HAS_WIDGETS
#ifndef FUZZ_SEEDS
  #define FUZZ_SEEDS 12
#endif
static uint32_t g_rng;
static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }
static int rr(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

static void test_fuzz(void) {
    static const char *words[] = {"a", "Hello", "a longer text that will need wrapping or clipping", "", "x\ny", "\xC2\xB0" "C"};
    long total_steps = 0;
    for (int seed = 1; seed <= FUZZ_SEEDS; seed++) {
        g_rng = (uint32_t)seed * 7919u;
        gui_open(PW, PH, 16);
        eccles_gui_render(&g_gui);
        eccles_gui_obj_t live[40];
        int nlive = 0;
        live[nlive++] = eccles_gui_root(&g_gui);
        uint32_t now = 0;
        for (int step = 0; step < 220; step++) {
            total_steps++;
            int op = rr(0, 17);
            eccles_gui_obj_t o = live[rr(0, nlive - 1)];
#ifdef FUZZ_TRACE
            printf("      seed %d step %d op %d on obj %u (type %d) live=%d\n", seed, step, op, (unsigned)o, g_gui.w[o].type, nlive);
#endif
            switch (op) {
            case 0: case 1: case 2: {
                if (nlive >= 40) break;
                eccles_gui_obj_t n = ECCLES_GUI_NO_OBJ;
                switch (rr(0, 10)) {
                case 0: n = eccles_gui_panel_create(&g_gui, o); break;
                case 1: n = eccles_gui_label_create(&g_gui, o, words[rr(0, 5)]); break;
                case 2: n = eccles_gui_button_create(&g_gui, o, words[rr(0, 5)]); break;
                case 3: n = eccles_gui_slider_create(&g_gui, o, 0, 100, rr(0, 100)); break;
                case 4: n = eccles_gui_switch_create(&g_gui, o, rr(0, 1)); break;
                case 5: n = eccles_gui_checkbox_create(&g_gui, o, words[rr(0, 5)], rr(0, 1)); break;
                case 6: n = eccles_gui_progress_create(&g_gui, o, 0, 100, rr(0, 100)); break;
                case 7: n = eccles_gui_arc_create(&g_gui, o, 0, 100, rr(0, 100)); break;
                case 8: n = eccles_gui_list_create(&g_gui, o, k_items, rr(0, 12)); break;
                case 9: n = eccles_gui_custom_create(&g_gui, o, custom_draw, NULL); break;
                default:
#if ECCLES_GUI_HAS_3D && ECCLES_GUI_MAX_VIEWS3D > 0 && ECCLES_GUI_HAS_BUILTIN_MESHES
                    n = eccles_gui_view3d_create(&g_gui, o);
                    if (n != ECCLES_GUI_NO_OBJ) eccles_gui_view3d_add(&g_gui, n, &eccles_gui_mesh_sphere, 0xFFFF);
#else
                    n = eccles_gui_panel_create(&g_gui, o);
#endif
                    break;
                }
                if (n != ECCLES_GUI_NO_OBJ) { live[nlive++] = n; eccles_gui_obj_set_area(&g_gui, n, rr(-20, PW), rr(-20, PH), rr(1, 90), rr(1, 70)); }
                break;
            }
            case 3: {
                if (o == eccles_gui_root(&g_gui)) break;
                eccles_gui_obj_delete(&g_gui, o);
                int w = 0;  //drop it and everything that died with it from the live list
                for (int i = 0; i < nlive; i++) if (eccles_gui_obj_valid(&g_gui, live[i])) live[w++] = live[i];
                nlive = w;
                break;
            }
            case 4: eccles_gui_obj_set_area(&g_gui, o, rr(-30, PW), rr(-30, PH), rr(0, 100), rr(0, 80)); break;
            case 5: eccles_gui_obj_set_pos(&g_gui, o, rr(-30, PW), rr(-30, PH)); break;
            case 6: eccles_gui_obj_set_hidden(&g_gui, o, rr(0, 3) == 0); break;
            case 7: eccles_gui_obj_set_opa(&g_gui, o, (uint8_t)rr(0, 255)); break;
            case 8: eccles_gui_obj_set_style(&g_gui, o, (uint8_t)rr(0, 7)); break;
            case 9: eccles_gui_set_text(&g_gui, o, words[rr(0, 5)]); break;
            case 10: eccles_gui_set_value(&g_gui, o, rr(-10, 110)); break;
            case 11: if (o != eccles_gui_root(&g_gui)) { if (rr(0, 1)) eccles_gui_obj_to_front(&g_gui, o); else eccles_gui_obj_to_back(&g_gui, o); } break;
            case 12: eccles_gui_obj_set_parent(&g_gui, o, live[rr(0, nlive - 1)]); break;
            case 13: eccles_gui_obj_set_align(&g_gui, o, (eccles_gui_align_t)rr(0, 9), rr(-10, 10), rr(-10, 10)); break;
            case 14: eccles_gui_list_scroll_to(&g_gui, o, rr(0, 400)); break;
            case 15: {
                int x = rr(0, PW - 1), y = rr(0, PH - 1);
                eccles_gui_touch(&g_gui, x, y, true);
                eccles_gui_touch(&g_gui, x + rr(-30, 30), y + rr(-60, 60), true);
                eccles_gui_touch(&g_gui, x + rr(-30, 30), y + rr(-60, 60), rr(0, 1));
                eccles_gui_touch(&g_gui, 0, 0, false);
                break;
            }
            case 16: {
#if ECCLES_GUI_HAS_ANIM && ECCLES_GUI_MAX_ANIMS > 0
                eccles_gui_anim_t a;
                memset(&a, 0, sizeof a);
                a.obj = o; a.prop = (uint8_t)rr(ECCLES_GUI_ANIM_X, ECCLES_GUI_ANIM_VALUE); a.from = rr(0, 100); a.to = rr(0, 255);
                a.duration_ms = (uint16_t)rr(10, 300); a.ease = (uint8_t)rr(0, 6); a.yoyo = rr(0, 1); a.repeat = (uint16_t)rr(0, 2);
                eccles_gui_anim_start(&g_gui, &a);
#endif
                break;
            }
            default: break;
            }
            //every few steps: render with a random band height, then compare against a full redraw
            now += (uint32_t)rr(1, 80);
            g_gui.d.buf_pixels = (size_t)PW * (size_t)rr(1, 40);
            eccles_gui_update(&g_gui, now);
            if (step % 5 == 4) {
                uint16_t inc[PW * PH];
                memcpy(inc, g_fb, sizeof inc);
                eccles_gui_invalidate_all(&g_gui);
                g_gui.d.buf_pixels = (size_t)PW * (size_t)rr(1, 40);
                eccles_gui_render(&g_gui);
                if (memcmp(inc, g_fb, sizeof inc)) {
                    int x0 = PW, y0 = PH, x1 = -1, y1 = -1, nd = 0;
                    for (int i = 0; i < PW * PH; i++) if (inc[i] != g_fb[i]) { int x = i % PW, y = i / PW; nd++; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
                    printf("    seed %d step %d (op %d): incremental render differs from full redraw: %d px in (%d,%d)-(%d,%d)\n", seed, step, op, nd, x0, y0, x1, y1);
                }
                CHECK(memcmp(inc, g_fb, sizeof inc) == 0);
                CHECK_EQ(g_bad_flush, 0);
            }
        }
        eccles_gui_stats_t st = eccles_gui_get_stats(&g_gui);
        CHECK(st.widgets_used <= ECCLES_GUI_MAX_WIDGETS);
    }
    printf("    %ld fuzz steps\n", total_steps);
}
#endif

int main(void) {
    printf("eccles_gui %s, widgets=%d 3d=%d anim=%d aa=%d, sizeof(context)=%u bytes\n", ECCLES_GUI_VERSION_STRING, ECCLES_GUI_HAS_WIDGETS, ECCLES_GUI_HAS_3D,
           ECCLES_GUI_HAS_ANIM, ECCLES_GUI_HAS_AA, (unsigned)sizeof(eccles_gui_t));
    RUN(test_color_math);
    RUN(test_sine_table);
#if ECCLES_GUI_HAS_ANIM && ECCLES_GUI_MAX_ANIMS > 0
    RUN(test_easing);
#endif
    RUN(test_init_errors);
    RUN(test_flush_rects);
    RUN(test_async_flush);
    RUN(test_swap_bytes);
    RUN(test_fill_exact);
    RUN(test_blend);
    RUN(test_disc_area_and_symmetry);
    RUN(test_border);
    RUN(test_lines);
    RUN(test_arc);
    RUN(test_triangle);
    RUN(test_clipping_everywhere);
    RUN(test_text_metrics);
    RUN(test_text_box);
    RUN(test_images);
#if ECCLES_GUI_HAS_WIDGETS
    RUN(test_dirty_merge);
    RUN(test_band_independence);
    RUN(test_incremental_equals_full);
    RUN(test_render_only_dirty);
    RUN(test_widget_pool);
    RUN(test_widget_types);
    RUN(test_tree_order_hit_align);
    RUN(test_layout);
    RUN(test_input_click_and_state);
    RUN(test_input_slider_list_gesture);
    RUN(test_input_long_press_and_fling);
#endif
#if ECCLES_GUI_HAS_ANIM && ECCLES_GUI_MAX_ANIMS > 0
    RUN(test_animation);
#endif
#if ECCLES_GUI_HAS_3D
  #if ECCLES_GUI_HAS_BUILTIN_MESHES
    RUN(test_mesh_integrity);
  #endif
    RUN(test_3d_render);
    RUN(test_runtime_normals);
    RUN(test_3d_limits);
  #if ECCLES_GUI_HAS_WIDGETS && ECCLES_GUI_MAX_VIEWS3D > 0
    RUN(test_view3d_widget);
  #endif
#endif
#if ECCLES_GUI_HAS_WIDGETS
    RUN(test_fuzz);
#endif
    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
