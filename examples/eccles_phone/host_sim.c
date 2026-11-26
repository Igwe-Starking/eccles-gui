// host_sim.c - runs the eccles phone ui on your pc at the real 750 x 1334, no hardware: a fake panel_hal collects the bands
// the engine flushes. it taps an icon, swipes the shade open, and writes phone.ppm (home) + phone_shade.ppm (shade open)
#include "eccles_phone_ui.h"
#include "eccles_gui_port_phone.h"
#include "panel_hal.h"
#include "panel_hal_raw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// the fake panel
static uint16_t *g_panel;
static uint32_t g_blits, g_blit_px;
void panel_hal_blit_raw(int x, int y, int w, int h, const uint16_t *px) {
    for (int j = 0; j < h; j++) memcpy(g_panel + (size_t)(y + j) * PANEL_WIDTH + (size_t)x, px + (size_t)j * (size_t)w, (size_t)w * 2u);
    g_blits++; g_blit_px += (uint32_t)(w * h);
}
void panel_hal_blit_raw_wait(void) {}
static int g_tx, g_ty;
static bool g_down;
void panel_hal_read_touch_raw(int *x, int *y, bool *pressed) { *x = g_tx; *y = g_ty; *pressed = g_down; }

static void save(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", PANEL_WIDTH, PANEL_HEIGHT);
    for (int i = 0; i < PANEL_WIDTH * PANEL_HEIGHT; i++) {
        unsigned p = g_panel[i], r = (p >> 11) & 31u, g = (p >> 5) & 63u, b = p & 31u;
        fputc((int)((r << 3) | (r >> 2)), f); fputc((int)((g << 2) | (g >> 4)), f); fputc((int)((b << 3) | (b >> 2)), f);
    }
    fclose(f);
}

static uint32_t g_now;
static eccles_gui_t g_gui;
static void frame(void) { g_now += 16; eccles_phone_gui_poll(&g_gui, g_now); }
static void touch(int x, int y, bool down) { g_tx = x; g_ty = y; g_down = down; frame(); }

static void launched(int i, void *u) { (void)u; printf("  -> launch app %d\n", i); }
static void nav(void *u) { (void)u; printf("  -> nav button\n"); }

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "phone.ppm";
    g_panel = (uint16_t *)calloc((size_t)PANEL_WIDTH * PANEL_HEIGHT, 2);
    if (!eccles_phone_gui_init(&g_gui)) { puts("init failed"); return 1; }
    eccles_phone_ui_hooks_t hooks = {launched, nav, nav, nav, NULL};
    if (!eccles_phone_ui_build(&g_gui, &hooks)) { printf("build failed: %s (%s)\n", eccles_gui_error_name(eccles_gui_last_error(&g_gui)), eccles_gui_last_error_str(&g_gui)); return 1; }
    eccles_phone_ui_set_time(&g_gui, 9, 41);
    eccles_phone_ui_set_notification_dot(&g_gui, true);
    frame();
    eccles_gui_stats_t s = eccles_gui_get_stats(&g_gui);
    printf("first frame: %u pixels in %u flushes, band buffer %u bytes, engine context %u bytes, %u widgets\n",
           g_blit_px, g_blits, (unsigned)(PANEL_WIDTH * ECCLES_PHONE_BAND_LINES * 2), s.ram_bytes, s.widgets_used);
    for (int i = 0; i < 30; i++) frame();  // let the 3d objects turn a bit
    g_blits = 0; g_blit_px = 0;
    frame();
    printf("idle frame with spinning 3d card: %u pixels (not the whole %d)\n", g_blit_px, PANEL_WIDTH * PANEL_HEIGHT);
    save(out);
    printf("wrote %s\n", out);
    // tap the Camera icon
    printf("tap Camera:\n");
    touch(60 + 240 * 2 + 75, 760 + 75, true); touch(60 + 240 * 2 + 75, 760 + 75, false);
    // swipe the shade open from the status bar
    printf("swipe down on the status bar:\n");
    touch(300, 10, true);
    for (int y = 10; y <= 300; y += 29) touch(300, y, true);
    touch(300, 320, false);
    for (int i = 0; i < 40; i++) frame();
    printf("shade open: %s\n", eccles_phone_ui_shade_open(&g_gui) ? "yes" : "NO");
    char shade_path[512];
    snprintf(shade_path, sizeof shade_path, "%.*s_shade.ppm", (int)(strlen(out) > 4 && strcmp(out + strlen(out) - 4, ".ppm") == 0 ? strlen(out) - 4 : strlen(out)), out);
    save(shade_path);
    // swipe it closed again
    touch(300, 500, true);
    for (int y = 500; y >= 200; y -= 30) touch(300, y, true);
    touch(300, 180, false);
    for (int i = 0; i < 40; i++) frame();
    printf("shade open after swipe up: %s\n", eccles_phone_ui_shade_open(&g_gui) ? "yes" : "no");
    free(g_panel);
    return 0;
}
