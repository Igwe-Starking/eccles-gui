// eccles_phone_ui.h - the system ui of the eccles phone built with eccles_gui: status bar, nav bar, home screen with an
// app grid, a 3d card, and the notification shade (swipe down from the status bar, swipe up to close).
// it is an EXAMPLE of how a real screen is put together, change it freely.
#ifndef ECCLES_PHONE_UI_H
#define ECCLES_PHONE_UI_H
#include "eccles_gui.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*on_launch)(int app_index, void *user);  // an icon was tapped
    void (*on_back)(void *user);
    void (*on_home)(void *user);
    void (*on_recents)(void *user);
    void *user;
} eccles_phone_ui_hooks_t;

// builds everything. the engine must be initialised for a 750 x 1334 panel (see eccles_gui_port_phone.c)
// needs ECCLES_GUI_MAX_WIDGETS >= 48 and ECCLES_GUI_MAX_STYLES >= 14 (the defaults) and one view3d
bool eccles_phone_ui_build(eccles_gui_t *gui, const eccles_phone_ui_hooks_t *hooks);
void eccles_phone_ui_set_time(eccles_gui_t *gui, int hour, int minute);
void eccles_phone_ui_set_battery(eccles_gui_t *gui, int percent);
void eccles_phone_ui_set_notification_dot(eccles_gui_t *gui, bool on);
bool eccles_phone_ui_shade_open(const eccles_gui_t *gui);

#ifdef __cplusplus
}
#endif
#endif
