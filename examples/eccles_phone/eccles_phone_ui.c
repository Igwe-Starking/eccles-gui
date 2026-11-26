// eccles_phone_ui.c - the eccles phone system ui, 750 x 1334, built with eccles_gui. read it top to bottom: it is the
// shortest way to see how widgets, styles, events, animation and the 3d view fit together.
#include "eccles_phone_ui.h"
#include "panel_hal.h"
#include <stdio.h>
#include <string.h>

#define SHADE_H 640

static struct {
    eccles_phone_ui_hooks_t hooks;
    eccles_gui_obj_t statusbar, clock, battery, dot, navbar, home, shade, shade_dim, v3, brightness;
    int big_font, small_font;
    char clock_txt[8], battery_txt[8];
    bool shade_open, dot_on;
    uint8_t icon_style[6];
} U;

static const char *const k_names[6] = {"Phone", "Messages", "Camera", "Gallery", "Store", "Apps"};
static const char k_letters[6][2] = {"P", "M", "C", "G", "S", "A"};
static const uint32_t k_colors[6] = {0x34C759, 0x3D8BFF, 0xFF9F0A, 0xBF5AF2, 0xFF453A, 0x64D2FF};

//the shade: a card that slides down over the home screen. an animation drives its y
static void set_shade_y(eccles_gui_t *gui, void *target, int32_t y) {
    (void)target;
    eccles_gui_obj_set_pos(gui, U.shade, 0, (int)y);
}
static void shade_slide(eccles_gui_t *gui, bool open) {
    U.shade_open = open;
    eccles_gui_anim_t a;
    memset(&a, 0, sizeof a);
    a.prop = ECCLES_GUI_ANIM_CUSTOM; a.exec = set_shade_y; a.target = &U;
    a.from = gui->w[U.shade].y;
    a.to = open ? PANEL_STATUS_BAR_H : -SHADE_H;
    a.duration_ms = 280; a.ease = open ? ECCLES_GUI_EASE_OUT_BACK : ECCLES_GUI_EASE_OUT_CUBIC;
    eccles_gui_anim_start(gui, &a);
    eccles_gui_obj_set_hidden(gui, U.shade_dim, !open);
}

static void on_statusbar(eccles_gui_t *gui, const eccles_gui_event_t *ev) {
    if (ev->code == ECCLES_GUI_EV_GESTURE && ev->value == ECCLES_GUI_DIR_DOWN) shade_slide(gui, true);
    if (ev->code == ECCLES_GUI_EV_CLICKED) shade_slide(gui, !U.shade_open);
}
static void on_shade(eccles_gui_t *gui, const eccles_gui_event_t *ev) {
    if (ev->code == ECCLES_GUI_EV_GESTURE && ev->value == ECCLES_GUI_DIR_UP) shade_slide(gui, false);
}
static void on_dim(eccles_gui_t *gui, const eccles_gui_event_t *ev) {
    if (ev->code == ECCLES_GUI_EV_CLICKED) shade_slide(gui, false);
}
static void on_icon(eccles_gui_t *gui, const eccles_gui_event_t *ev) {
    (void)gui;
    if (ev->code == ECCLES_GUI_EV_CLICKED && U.hooks.on_launch) U.hooks.on_launch((int)(intptr_t)ev->user, U.hooks.user);
}
static void on_nav(eccles_gui_t *gui, const eccles_gui_event_t *ev) {
    (void)gui;
    if (ev->code != ECCLES_GUI_EV_CLICKED) return;
    int which = (int)(intptr_t)ev->user;
    if (which == 0 && U.hooks.on_back) U.hooks.on_back(U.hooks.user);
    if (which == 1 && U.hooks.on_home) U.hooks.on_home(U.hooks.user);
    if (which == 2 && U.hooks.on_recents) U.hooks.on_recents(U.hooks.user);
}
//dragging the 3d card turns the cube
static void on_spin(eccles_gui_t *gui, const eccles_gui_event_t *ev) {
    if (ev->code != ECCLES_GUI_EV_DRAG) return;
    eccles_gui_view3d_t *s = eccles_gui_view3d_get(gui, U.v3);
    if (!s) return;
    s->obj[0].yaw = (uint16_t)(s->obj[0].yaw - ev->dx * 120);
    s->obj[0].pitch = (uint16_t)(s->obj[0].pitch + ev->dy * 120);
    eccles_gui_view3d_changed(gui, U.v3);
}

//custom widgets: the three nav bar buttons and the notification dot
static void draw_nav(eccles_gui_dc_t *dc, const eccles_gui_area_t *a, void *user) {
    int which = (int)(intptr_t)user, cx = (a->x1 + a->x2) / 2, cy = (a->y1 + a->y2) / 2;
    uint16_t c = 0xE71C;
    if (which == 0) eccles_gui_draw_triangle(dc, cx + 14, cy - 18, cx + 14, cy + 18, cx - 16, cy, c, 255);
    else if (which == 1) eccles_gui_draw_border(dc, cx - 18, cy - 18, 36, 36, 18, 5, c, 255);
    else eccles_gui_draw_border(dc, cx - 16, cy - 16, 32, 32, 8, 5, c, 255);
}
static void draw_dot(eccles_gui_dc_t *dc, const eccles_gui_area_t *a, void *user) {
    (void)user;
    if (U.dot_on) eccles_gui_draw_disc(dc, (a->x1 + a->x2) / 2, (a->y1 + a->y2) / 2, 8, 0xFD20, 255);
}

static eccles_gui_obj_t make_label(eccles_gui_t *gui, eccles_gui_obj_t parent, const char *t, uint8_t style, int x, int y) {
    eccles_gui_obj_t l = eccles_gui_label_create(gui, parent, t);
    eccles_gui_obj_set_style(gui, l, style);
    eccles_gui_obj_set_pos(gui, l, x, y);
    return l;
}

bool eccles_phone_ui_build(eccles_gui_t *gui, const eccles_phone_ui_hooks_t *hooks) {
    memset(&U, 0, sizeof U);
    if (hooks) U.hooks = *hooks;
    eccles_gui_obj_t root = eccles_gui_root(gui);
    //fonts: the built in 5x8 font at 8x for the big clock, 2x for small text
    U.big_font = eccles_gui_font_register(gui, &eccles_gui_font_5x8, 8);
    U.small_font = ECCLES_GUI_FONT_2X;
    //styles: a status bar text style, and one colored button style per app icon
    eccles_gui_style_t st = *eccles_gui_style_get(gui, ECCLES_GUI_STYLE_LABEL);
    st.font = (uint8_t)U.small_font;
    int bar_text = eccles_gui_style_add(gui, &st);
    st.font = (uint8_t)U.big_font;
    int clock_style = eccles_gui_style_add(gui, &st);
    for (int i = 0; i < 6; i++) {
        st = *eccles_gui_style_get(gui, ECCLES_GUI_STYLE_BUTTON_PRIMARY);
        st.bg = ECCLES_GUI_HEX(k_colors[i]);
        st.bg2 = eccles_gui_color_mix(st.bg, 0, 90);
        st.radius = 38; st.border_w = 0; st.font = ECCLES_GUI_FONT_4X; st.text = 0xFFFF; st.shadow = 8;
        int s = eccles_gui_style_add(gui, &st);
        if (s < 0) return false;
        U.icon_style[i] = (uint8_t)s;
    }
    if (bar_text < 0 || clock_style < 0 || U.big_font < 0) return false;

    //home screen (between the bars)
    U.home = eccles_gui_panel_create(gui, root);
    eccles_gui_obj_set_area(gui, U.home, 0, PANEL_STATUS_BAR_H, PANEL_WIDTH, PANEL_APP_AREA_H);
    snprintf(U.clock_txt, sizeof U.clock_txt, "9:41");
    eccles_gui_obj_t clock = make_label(gui, U.home, U.clock_txt, (uint8_t)clock_style, 44, 20);
    eccles_gui_obj_t date = make_label(gui, U.home, "Saturday, 21 November", ECCLES_GUI_STYLE_LABEL_MUTED, 50, 118);
    (void)clock; (void)date;

    //3d card: drag to spin the cube
    eccles_gui_obj_t card = eccles_gui_panel_create(gui, U.home);
    eccles_gui_obj_set_style(gui, card, ECCLES_GUI_STYLE_CARD);
    eccles_gui_obj_set_area(gui, card, 30, 170, 690, 330);
    U.v3 = eccles_gui_view3d_create(gui, card);
    if (U.v3 == ECCLES_GUI_NO_OBJ) return false;
    eccles_gui_obj_set_area(gui, U.v3, 12, 12, 666, 306);
    eccles_gui_obj_on(gui, U.v3, on_spin, NULL);
    eccles_gui_view3d_t *s = eccles_gui_view3d_get(gui, U.v3);
    s->bg = ECCLES_GUI_HEX(0x121821);
    s->focal = 330; s->cam_z = -8 * ECCLES_GUI_ONE; s->cam_y = ECCLES_GUI_ONE / 3;
    int c0 = eccles_gui_view3d_add(gui, U.v3, &eccles_gui_mesh_cube, ECCLES_GUI_HEX(0x3D8BFF));
    int c1 = eccles_gui_view3d_add(gui, U.v3, &eccles_gui_mesh_sphere, ECCLES_GUI_HEX(0xFF6B6B));
    int c2 = eccles_gui_view3d_add(gui, U.v3, &eccles_gui_mesh_torus, ECCLES_GUI_HEX(0x4ADE80));
    eccles_gui_view3d_set_transform(gui, U.v3, c0, -3 * ECCLES_GUI_ONE - ECCLES_GUI_ONE / 3, 0, 0, ECCLES_GUI_DEG(30), ECCLES_GUI_DEG(25), 0, ECCLES_GUI_ONE * 5 / 4);
    eccles_gui_view3d_set_transform(gui, U.v3, c1, 0, 0, 0, 0, 0, 0, ECCLES_GUI_ONE * 5 / 4);
    eccles_gui_view3d_set_transform(gui, U.v3, c2, 3 * ECCLES_GUI_ONE + ECCLES_GUI_ONE / 3, 0, 0, 0, ECCLES_GUI_DEG(65), 0, ECCLES_GUI_ONE * 5 / 4);
    eccles_gui_view3d_set_mode(gui, U.v3, c1, ECCLES_GUI_3D_SMOOTH);
    eccles_gui_view3d_set_mode(gui, U.v3, c2, ECCLES_GUI_3D_SMOOTH);
    eccles_gui_view3d_set_light(gui, U.v3, -40, 60, -80, 70);
    for (int i = 1; i < 3; i++) {  //the sphere and the torus turn on their own, forever
        eccles_gui_anim_t a;
        memset(&a, 0, sizeof a);
        a.obj = U.v3; a.prop = ECCLES_GUI_ANIM_V3D_YAW; a.index = (uint8_t)(i == 1 ? 1 : 2);
        a.from = 0; a.to = 65536; a.duration_ms = (uint16_t)(i == 1 ? 9000 : 6000); a.repeat = ECCLES_GUI_ANIM_REPEAT_FOREVER;
        eccles_gui_anim_start(gui, &a);
    }

    //quick settings card
    eccles_gui_obj_t q = eccles_gui_panel_create(gui, U.home);
    eccles_gui_obj_set_style(gui, q, ECCLES_GUI_STYLE_CARD);
    eccles_gui_obj_set_area(gui, q, 30, 530, 690, 190);
    make_label(gui, q, "Brightness", ECCLES_GUI_STYLE_LABEL, 28, 22);
    U.brightness = eccles_gui_slider_create(gui, q, 0, 100, 70);
    eccles_gui_obj_set_area(gui, U.brightness, 28, 76, 634, 54);
    eccles_gui_obj_t prog = eccles_gui_progress_create(gui, q, 0, 100, 87);
    eccles_gui_obj_set_area(gui, prog, 28, 150, 634, 16);

    //app grid
    for (int i = 0; i < 6; i++) {
        int x = 60 + (i % 3) * 240, y = 760 + (i / 3) * 235;
        eccles_gui_obj_t b = eccles_gui_button_create(gui, U.home, k_letters[i]);
        eccles_gui_obj_set_style(gui, b, U.icon_style[i]);
        eccles_gui_obj_set_area(gui, b, x, y, 150, 150);
        eccles_gui_obj_on(gui, b, on_icon, (void *)(intptr_t)i);
        eccles_gui_obj_t l = eccles_gui_label_create(gui, U.home, k_names[i]);
        eccles_gui_obj_set_pos(gui, l, x + 75 - eccles_gui_obj_get_area(gui, l).x2 / 2 + eccles_gui_obj_get_area(gui, l).x1 / 2 - 0, y + 160);
        eccles_gui_obj_set_pos(gui, l, x + 75 - (eccles_gui_obj_get_area(gui, l).x2 - eccles_gui_obj_get_area(gui, l).x1) / 2, y + 160);
    }

    //the shade, closed: parked above the screen. below it a dim layer that closes it when tapped
    U.shade_dim = eccles_gui_panel_create(gui, root);
    eccles_gui_obj_set_area(gui, U.shade_dim, 0, PANEL_STATUS_BAR_H, PANEL_WIDTH, PANEL_HEIGHT - PANEL_STATUS_BAR_H);
    eccles_gui_obj_set_clickable(gui, U.shade_dim, true);
    eccles_gui_obj_on(gui, U.shade_dim, on_dim, NULL);
    eccles_gui_obj_set_hidden(gui, U.shade_dim, true);
    U.shade = eccles_gui_panel_create(gui, root);
    eccles_gui_obj_set_style(gui, U.shade, ECCLES_GUI_STYLE_CARD);
    eccles_gui_obj_set_area(gui, U.shade, 0, -SHADE_H, PANEL_WIDTH, SHADE_H);
    eccles_gui_obj_set_clickable(gui, U.shade, true);
    eccles_gui_obj_on(gui, U.shade, on_shade, NULL);
    make_label(gui, U.shade, "Quick settings", ECCLES_GUI_STYLE_LABEL_TITLE, 40, 40);
    static const char *const names[4] = {"Wi-Fi", "Bluetooth", "Airplane", "Do not disturb"};
    for (int i = 0; i < 4; i++) {
        int y = 140 + i * 90;
        make_label(gui, U.shade, names[i], ECCLES_GUI_STYLE_LABEL, 40, y + 8);
        eccles_gui_obj_t sw = eccles_gui_switch_create(gui, U.shade, i < 2);
        eccles_gui_obj_set_area(gui, sw, 560, y, 120, 60);
    }
    eccles_gui_obj_t grip = eccles_gui_panel_create(gui, U.shade);
    eccles_gui_obj_set_style(gui, grip, ECCLES_GUI_STYLE_BUTTON);
    eccles_gui_obj_set_area(gui, grip, PANEL_WIDTH / 2 - 60, SHADE_H - 30, 120, 10);

    //status bar and nav bar: always on top
    U.statusbar = eccles_gui_panel_create(gui, root);
    eccles_gui_obj_set_area(gui, U.statusbar, 0, 0, PANEL_WIDTH, PANEL_STATUS_BAR_H);
    eccles_gui_obj_set_style(gui, U.statusbar, ECCLES_GUI_STYLE_SCREEN);
    eccles_gui_obj_set_clickable(gui, U.statusbar, true);
    eccles_gui_obj_on(gui, U.statusbar, on_statusbar, NULL);
    U.clock = make_label(gui, U.statusbar, U.clock_txt, (uint8_t)bar_text, 20, 9);
    snprintf(U.battery_txt, sizeof U.battery_txt, "87%%");
    U.battery = make_label(gui, U.statusbar, U.battery_txt, (uint8_t)bar_text, PANEL_WIDTH - 100, 9);
    U.dot = eccles_gui_custom_create(gui, U.statusbar, draw_dot, NULL);
    eccles_gui_obj_set_area(gui, U.dot, PANEL_WIDTH / 2 - 10, 8, 20, 20);

    U.navbar = eccles_gui_panel_create(gui, root);
    eccles_gui_obj_set_area(gui, U.navbar, 0, PANEL_HEIGHT - PANEL_NAV_BAR_H, PANEL_WIDTH, PANEL_NAV_BAR_H);
    eccles_gui_obj_set_style(gui, U.navbar, ECCLES_GUI_STYLE_SCREEN);
    for (int i = 0; i < 3; i++) {
        eccles_gui_obj_t b = eccles_gui_custom_create(gui, U.navbar, draw_nav, (void *)(intptr_t)i);
        eccles_gui_obj_set_area(gui, b, PANEL_WIDTH / 6 + i * (PANEL_WIDTH / 3) - 80, 0, 160, PANEL_NAV_BAR_H);
        eccles_gui_obj_set_clickable(gui, b, true);
        eccles_gui_obj_on(gui, b, on_nav, (void *)(intptr_t)i);
    }
    return true;
}

void eccles_phone_ui_set_time(eccles_gui_t *gui, int hour, int minute) {
    snprintf(U.clock_txt, sizeof U.clock_txt, "%d:%02d", hour, minute);
    eccles_gui_set_text(gui, U.clock, U.clock_txt);  // same buffer: set_text again to redraw
    for (eccles_gui_obj_t c = eccles_gui_obj_child(gui, U.home); c != ECCLES_GUI_NO_OBJ; c = eccles_gui_obj_next(gui, c)) {
        if (gui->w[c].type == ECCLES_GUI_W_LABEL && gui->w[c].u.label.text == U.clock_txt) eccles_gui_set_text(gui, c, U.clock_txt);
    }
}
void eccles_phone_ui_set_battery(eccles_gui_t *gui, int percent) {
    snprintf(U.battery_txt, sizeof U.battery_txt, "%d%%", percent);
    eccles_gui_set_text(gui, U.battery, U.battery_txt);
}
void eccles_phone_ui_set_notification_dot(eccles_gui_t *gui, bool on) {
    U.dot_on = on;
    eccles_gui_obj_invalidate(gui, U.dot);
}
bool eccles_phone_ui_shade_open(const eccles_gui_t *gui) { (void)gui; return U.shade_open; }
