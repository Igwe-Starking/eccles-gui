//eccles_gui host demo: builds a phone style screen (2d widgets + three 3d objects) and renders it into a virtual panel
//no hardware needed. it writes demo.ppm (turn it into png with tools/ppm2png.py)
//
//  cc -std=c99 -O2 -Isrc -Iconfig examples/host/demo.c src/eccles_gui.c -o demo && ./demo demo.ppm [width height]

#include "eccles_gui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//the virtual panel: a plain rgb565 array. a real driver would send the band over spi / rgb / dma here
static uint16_t *g_panel;
static int g_w, g_h;
static uint32_t g_flushes;

static void panel_flush(void *user, int x, int y, int w, int h, const uint16_t *px) {
    (void)user;
    for (int j = 0; j < h; j++) memcpy(g_panel + (size_t)(y + j) * (size_t)g_w + (size_t)x, px + (size_t)j * (size_t)w, (size_t)w * 2u);
    g_flushes++;
}

static void save_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", g_w, g_h);
    for (int i = 0; i < g_w * g_h; i++) {
        uint16_t p = g_panel[i];
        unsigned r = (p >> 11) & 31u, g = (p >> 5) & 63u, b = p & 31u;
        fputc((int)((r << 3) | (r >> 2)), f); fputc((int)((g << 2) | (g >> 4)), f); fputc((int)((b << 3) | (b >> 2)), f);
    }
    fclose(f);
}

static const char *const g_items[] = {"Wi-Fi", "Bluetooth", "Display", "Sound", "Battery", "Storage", "Apps", "About phone"};

static void on_click(eccles_gui_t *gui, const eccles_gui_event_t *ev) {
    (void)gui;
    printf("event %d on widget %u value %d\n", (int)ev->code, (unsigned)ev->obj, (int)ev->value);
}

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "demo.ppm";
    g_w = argc > 3 ? atoi(argv[2]) : 375;
    g_h = argc > 3 ? atoi(argv[3]) : 667;
    g_panel = (uint16_t *)calloc((size_t)g_w * (size_t)g_h, 2);
    //the band buffer: 16 lines. this is ALL the pixel memory the engine needs
    static uint16_t band[1024 * 16];
    static eccles_gui_t gui;  //the whole ui lives in here, no malloc
    eccles_gui_desc_t d;
    memset(&d, 0, sizeof d);
    d.width = (int16_t)g_w; d.height = (int16_t)g_h;
    d.buf = band; d.buf_pixels = (size_t)g_w * 16;
    d.flush = panel_flush;
    if (eccles_gui_init(&gui, &d) != ECCLES_GUI_SUCCESSFUL) { fprintf(stderr, "init failed: %s\n", eccles_gui_error_name(eccles_gui_last_error(&gui))); return 1; }

    int u = g_w / 125;  //ui unit
    if (u < 1) u = 1;
    eccles_gui_obj_t root = eccles_gui_root(&gui);

    eccles_gui_obj_t title = eccles_gui_label_create(&gui, root, "Eccles Phone");
    eccles_gui_obj_set_style(&gui, title, ECCLES_GUI_STYLE_LABEL_TITLE);
    eccles_gui_obj_set_pos(&gui, title, 6 * u, 6 * u);
    eccles_gui_obj_t sub = eccles_gui_label_create(&gui, root, "eccles-gui 1.0.0 - 2d + 3d, no framebuffer");
    eccles_gui_obj_set_style(&gui, sub, ECCLES_GUI_STYLE_LABEL_MUTED);
    eccles_gui_obj_set_pos(&gui, sub, 6 * u, 6 * u + 14 * u);

    //3d card
    eccles_gui_obj_t card = eccles_gui_panel_create(&gui, root);
    eccles_gui_obj_set_style(&gui, card, ECCLES_GUI_STYLE_CARD);
    eccles_gui_obj_set_area(&gui, card, 6 * u, 26 * u, g_w - 12 * u, 50 * u);
    eccles_gui_obj_t v3 = eccles_gui_view3d_create(&gui, card);
    eccles_gui_obj_set_area(&gui, v3, 3 * u, 3 * u, g_w - 18 * u, 44 * u);
    eccles_gui_view3d_t *scene = eccles_gui_view3d_get(&gui, v3);
    scene->bg = ECCLES_GUI_HEX(0x141B26);
    scene->focal = (int16_t)(44 * u);
    scene->cam_z = -7 * ECCLES_GUI_ONE;
    scene->cam_y = ECCLES_GUI_ONE / 2;
    int c0 = eccles_gui_view3d_add(&gui, v3, &eccles_gui_mesh_cube, ECCLES_GUI_HEX(0x3D8BFF));
    int c1 = eccles_gui_view3d_add(&gui, v3, &eccles_gui_mesh_sphere, ECCLES_GUI_HEX(0xFF6B6B));
    int c2 = eccles_gui_view3d_add(&gui, v3, &eccles_gui_mesh_torus, ECCLES_GUI_HEX(0x4ADE80));
    eccles_gui_view3d_set_transform(&gui, v3, c0, -3 * ECCLES_GUI_ONE, 0, 0, ECCLES_GUI_DEG(30), ECCLES_GUI_DEG(25), 0, ECCLES_GUI_ONE);
    eccles_gui_view3d_set_transform(&gui, v3, c1, 0, 0, 0, 0, 0, 0, ECCLES_GUI_ONE);
    eccles_gui_view3d_set_transform(&gui, v3, c2, 3 * ECCLES_GUI_ONE, 0, 0, ECCLES_GUI_DEG(20), ECCLES_GUI_DEG(70), 0, ECCLES_GUI_ONE);
    eccles_gui_view3d_set_mode(&gui, v3, c1, ECCLES_GUI_3D_SMOOTH);
    eccles_gui_view3d_set_mode(&gui, v3, c2, ECCLES_GUI_3D_SMOOTH);

    //buttons
    eccles_gui_obj_t b1 = eccles_gui_button_create(&gui, root, "Settings");
    eccles_gui_obj_set_pos(&gui, b1, 6 * u, 82 * u);
    eccles_gui_obj_on(&gui, b1, on_click, NULL);
    eccles_gui_obj_t b2 = eccles_gui_button_create(&gui, root, "Call");
    eccles_gui_obj_set_style(&gui, b2, ECCLES_GUI_STYLE_BUTTON_PRIMARY);
    eccles_gui_obj_set_pos(&gui, b2, 6 * u + eccles_gui_obj_get_area(&gui, b1).x2 - eccles_gui_obj_get_area(&gui, b1).x1 + 4 * u, 82 * u);

    //controls card
    eccles_gui_obj_t c = eccles_gui_panel_create(&gui, root);
    eccles_gui_obj_set_style(&gui, c, ECCLES_GUI_STYLE_CARD);
    eccles_gui_obj_set_area(&gui, c, 6 * u, 96 * u, g_w - 12 * u, 36 * u);
    eccles_gui_obj_t sw = eccles_gui_switch_create(&gui, c, true);
    eccles_gui_obj_set_area(&gui, sw, 4 * u, 4 * u, 18 * u, 9 * u);
    eccles_gui_obj_t cb = eccles_gui_checkbox_create(&gui, c, "Airplane", false);
    eccles_gui_obj_set_pos(&gui, cb, 28 * u, 4 * u);
    eccles_gui_obj_t sl = eccles_gui_slider_create(&gui, c, 0, 100, 65);
    eccles_gui_obj_set_area(&gui, sl, 4 * u, 18 * u, g_w - 20 * u, 8 * u);
    eccles_gui_obj_t pr = eccles_gui_progress_create(&gui, c, 0, 100, 40);
    eccles_gui_obj_set_area(&gui, pr, 4 * u, 29 * u, g_w - 20 * u, 3 * u);

    //gauge + list
    eccles_gui_obj_t arc = eccles_gui_arc_create(&gui, root, 0, 100, 72);
    eccles_gui_obj_set_area(&gui, arc, 6 * u, 138 * u, 40 * u, 40 * u);
    eccles_gui_arc_set_thickness(&gui, arc, 4 * u);
    eccles_gui_obj_t al = eccles_gui_label_create(&gui, arc, "72%");
    eccles_gui_obj_set_align(&gui, al, ECCLES_GUI_ALIGN_CENTER, 0, 0);
    eccles_gui_obj_t list = eccles_gui_list_create(&gui, root, g_items, 8);
    eccles_gui_obj_set_area(&gui, list, 52 * u, 138 * u, g_w - 58 * u, 40 * u);
    eccles_gui_list_set_selected(&gui, list, 2);

    uint32_t px = eccles_gui_update(&gui, 0);
    eccles_gui_stats_t s = eccles_gui_get_stats(&gui);
    printf("rendered %u pixels in %u flushes (panel %dx%d), band buffer = %u bytes, context = %u bytes, widgets %u\n",
           px, g_flushes, g_w, g_h, (unsigned)(d.buf_pixels * 2), s.ram_bytes, s.widgets_used);
    save_ppm(out);
    printf("wrote %s\n", out);
    free(g_panel);
    return 0;
}
