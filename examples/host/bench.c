#define _POSIX_C_SOURCE 199309L
//eccles_gui host benchmark: how long does a frame take on THIS machine. numbers from a pc say little about an mcu, they are
//there to compare configurations and to catch performance regressions. what to look at is pixels per frame and the ratios
//
//  cc -std=c99 -O2 -Isrc -Iconfig examples/host/bench.c src/eccles_gui.c -o bench && ./bench

#include "eccles_gui.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define W 375
#define H 667
static uint16_t band[W * 16], zbuf[W * 16];
static uint32_t sink;
static void flush(void *u, int x, int y, int w, int h, const uint16_t *p) { (void)u; (void)x; (void)y; sink += p[(size_t)w * (size_t)h / 2]; }

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }

static void bench_full(eccles_gui_t *g, const char *name, int n) {
    uint32_t px = 0;
    double t0 = now_ms();
    for (int i = 0; i < n; i++) { eccles_gui_invalidate_all(g); px = eccles_gui_render(g); }
    double ms = (now_ms() - t0) / n;
    printf("%-34s %8.3f ms/frame   %7u px   %6.1f Mpx/s\n", name, ms, px, px / ms / 1000.0);
}

int main(void) {
    static eccles_gui_t g;
    eccles_gui_desc_t d;
    memset(&d, 0, sizeof d);
    d.width = W; d.height = H; d.buf = band; d.buf_pixels = W * 16; d.flush = flush;
    eccles_gui_init(&g, &d);
    eccles_gui_obj_t root = eccles_gui_root(&g);

    //1. flat screen
    bench_full(&g, "empty screen (just the root)", 200);

    //2. a typical settings screen: cards, labels, buttons, switches, a list
    static const char *items[] = {"Wi-Fi", "Bluetooth", "Display", "Sound", "Battery", "Storage", "Apps", "About"};
    for (int i = 0; i < 3; i++) {
        eccles_gui_obj_t c = eccles_gui_panel_create(&g, root);
        eccles_gui_obj_set_style(&g, c, ECCLES_GUI_STYLE_CARD);
        eccles_gui_obj_set_area(&g, c, 10, 10 + i * 130, W - 20, 120);
        eccles_gui_obj_t l = eccles_gui_label_create(&g, c, "Card title and some text");
        eccles_gui_obj_set_pos(&g, l, 8, 8);
        eccles_gui_obj_t b = eccles_gui_button_create(&g, c, "Button");
        eccles_gui_obj_set_pos(&g, b, 8, 40);
        eccles_gui_obj_t s = eccles_gui_switch_create(&g, c, i & 1);
        eccles_gui_obj_set_area(&g, s, 200, 40, 60, 30);
        eccles_gui_obj_t sl = eccles_gui_slider_create(&g, c, 0, 100, 40 + i * 20);
        eccles_gui_obj_set_area(&g, sl, 8, 85, W - 50, 24);
    }
    eccles_gui_obj_t list = eccles_gui_list_create(&g, root, items, 8);
    eccles_gui_obj_set_area(&g, list, 10, 410, W - 20, 150);
    eccles_gui_obj_t arc = eccles_gui_arc_create(&g, root, 0, 100, 70);
    eccles_gui_obj_set_area(&g, arc, 10, 570, 90, 90);
    bench_full(&g, "settings screen (2d widgets)", 100);
    eccles_gui_set_value(&g, arc, 71);
    uint32_t px = 0;
    double t0 = now_ms();
    for (int i = 0; i < 1000; i++) { eccles_gui_set_value(&g, arc, 40 + (i & 31)); px = eccles_gui_render(&g); }
    double ms = (now_ms() - t0) / 1000;
    printf("%-34s %8.3f ms/frame   %7u px   (only the gauge redraws)\n", "gauge value changes", ms, px);

    //3. 3d
    eccles_gui_obj_t v = eccles_gui_view3d_create(&g, root);
    eccles_gui_obj_set_area(&g, v, 110, 570, W - 120, 90);
    eccles_gui_view3d_t *s = eccles_gui_view3d_get(&g, v);
    s->focal = 90; s->cam_z = -6 * ECCLES_GUI_ONE;
    int a = eccles_gui_view3d_add(&g, v, &eccles_gui_mesh_torus, 0x3D9F);
    int b = eccles_gui_view3d_add(&g, v, &eccles_gui_mesh_sphere, 0xFB4A);
    eccles_gui_view3d_set_mode(&g, v, a, ECCLES_GUI_3D_SMOOTH);
    eccles_gui_view3d_set_mode(&g, v, b, ECCLES_GUI_3D_SMOOTH);
    eccles_gui_view3d_set_transform(&g, v, a, -ECCLES_GUI_ONE * 2, 0, 0, 0, ECCLES_GUI_DEG(60), 0, ECCLES_GUI_ONE);
    eccles_gui_view3d_set_transform(&g, v, b, ECCLES_GUI_ONE * 2, 0, 0, 0, 0, 0, ECCLES_GUI_ONE);
    t0 = now_ms();
    for (int i = 0; i < 1000; i++) {
        eccles_gui_view3d_set_transform(&g, v, a, -ECCLES_GUI_ONE * 2, 0, 0, (uint16_t)(i * 700), ECCLES_GUI_DEG(60), 0, ECCLES_GUI_ONE);
        px = eccles_gui_render(&g);
    }
    ms = (now_ms() - t0) / 1000;
    printf("%-34s %8.3f ms/frame   %7u px   (%u faces drawn, torus + sphere, smooth, painter)\n", "spinning 3d view (90 x 255 px)", ms, px, eccles_gui_get_stats(&g).faces_drawn);
    d.zbuf = zbuf;
    eccles_gui_init(&g, &d);
    root = eccles_gui_root(&g);
    v = eccles_gui_view3d_create(&g, root);
    eccles_gui_obj_set_area(&g, v, 10, 10, W - 20, 300);
    s = eccles_gui_view3d_get(&g, v);
    s->focal = 300; s->cam_z = -6 * ECCLES_GUI_ONE;
    a = eccles_gui_view3d_add(&g, v, &eccles_gui_mesh_torus, 0x3D9F);
    eccles_gui_view3d_set_mode(&g, v, a, ECCLES_GUI_3D_SMOOTH);
    eccles_gui_view3d_set_transform(&g, v, a, 0, 0, 0, 0, ECCLES_GUI_DEG(60), 0, ECCLES_GUI_ONE * 2);
    t0 = now_ms();
    for (int i = 0; i < 300; i++) {
        eccles_gui_view3d_set_transform(&g, v, a, 0, 0, 0, (uint16_t)(i * 700), ECCLES_GUI_DEG(60), 0, ECCLES_GUI_ONE * 2);
        px = eccles_gui_render(&g);
    }
    ms = (now_ms() - t0) / 300;
    printf("%-34s %8.3f ms/frame   %7u px   (%u faces drawn, big torus, smooth, z buffer)\n", "3d view 355 x 300 with z buffer", ms, px, eccles_gui_get_stats(&g).faces_drawn);
    if (sink == 12345) puts("");
    return 0;
}
