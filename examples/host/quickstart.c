//the quick start from the README, as a program you can run: a label, a button that changes its text when clicked, and a spinning cube.
//it "taps" the button with a fake touch and writes quickstart.ppm
//  cc -std=c99 -O2 -Isrc -Iconfig examples/host/quickstart.c src/eccles_gui.c -o quickstart && ./quickstart quickstart.ppm

#include "eccles_gui.h"
#include <stdio.h>
#include <string.h>

#define W 320
#define H 480
static uint16_t band[320 * 16];          // 16 lines of a 320 px wide panel = 10 KB
static eccles_gui_t gui;                 // the whole UI lives in here, no malloc
static uint16_t panel[W * H];            // a pc has room to keep the result so we can save it. an mcu does not need this

static void my_panel_write(int x, int y, int w, int h, const uint16_t *px) {
    for (int j = 0; j < h; j++) memcpy(panel + (y + j) * W + x, px + j * w, (size_t)w * 2);
}
static void flush(void *user, int x, int y, int w, int h, const uint16_t *px) {
    (void)user;
    my_panel_write(x, y, w, h, px);      // <- your display driver. rgb565, w*h pixels, row after row
}

static void on_button(eccles_gui_t *g, const eccles_gui_event_t *ev) {
    if (ev->code == ECCLES_GUI_EV_CLICKED) eccles_gui_set_text(g, ev->obj, "Thanks!");
}

int main(int argc, char **argv) {
    eccles_gui_desc_t d = {0};
    d.width = W; d.height = H;
    d.buf = band; d.buf_pixels = W * 16;
    d.flush = flush;
    if (eccles_gui_init(&gui, &d) != ECCLES_GUI_SUCCESSFUL) return 1;

    eccles_gui_obj_t root  = eccles_gui_root(&gui);
    eccles_gui_obj_t title = eccles_gui_label_create(&gui, root, "Hello");
    eccles_gui_obj_set_style(&gui, title, ECCLES_GUI_STYLE_LABEL_TITLE);
    eccles_gui_obj_set_pos(&gui, title, 10, 10);

    eccles_gui_obj_t btn = eccles_gui_button_create(&gui, root, "Press me");
    eccles_gui_obj_set_pos(&gui, btn, 10, 60);
    eccles_gui_obj_on(&gui, btn, on_button, NULL);

    //a 3D widget in six lines
    eccles_gui_obj_t v = eccles_gui_view3d_create(&gui, root);
    eccles_gui_obj_set_area(&gui, v, 10, 120, 300, 200);
    int cube = eccles_gui_view3d_add(&gui, v, &eccles_gui_mesh_cube, ECCLES_GUI_HEX(0x3D8BFF));
    eccles_gui_anim_t a = {0};
    a.obj = v; a.prop = ECCLES_GUI_ANIM_V3D_YAW; a.index = (uint8_t)cube;
    a.from = 0; a.to = 65536; a.duration_ms = 4000; a.repeat = ECCLES_GUI_ANIM_REPEAT_FOREVER;
    eccles_gui_anim_start(&gui, &a);

    //the main loop, with a fake touch panel: a tap on the button at t = 300 ms
    for (uint32_t t = 0; t < 1000; t += 16) {
        bool down = t >= 300 && t < 340;
        eccles_gui_touch(&gui, 40, 75, down);
        eccles_gui_update(&gui, t);
    }
    printf("button text is now \"%s\"\n", gui.w[btn].u.label.text);
    if (strcmp(gui.w[btn].u.label.text, "Thanks!") != 0) return 2;
    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (int i = 0; i < W * H; i++) { unsigned p = panel[i]; fputc((int)(((p >> 11) & 31u) << 3), f); fputc((int)(((p >> 5) & 63u) << 2), f); fputc((int)((p & 31u) << 3), f); }
        fclose(f);
    }
    return 0;
}
