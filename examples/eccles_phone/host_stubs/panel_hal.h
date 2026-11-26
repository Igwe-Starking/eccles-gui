// host_stubs/panel_hal.h - only for building the example on a pc.
// the constants and signatures below are copied from components/hal/include/panel_hal.h of the eccles phone project
// (the real header is used when you build inside the project). keep them in sync if you change your panel.
#ifndef ECCLES_PHONE_PANEL_HAL_H
#define ECCLES_PHONE_PANEL_HAL_H
#include <stdint.h>
#include <stdbool.h>
#define PANEL_WIDTH         750
#define PANEL_HEIGHT        1334
#define PANEL_STATUS_BAR_H  36
#define PANEL_NAV_BAR_H     90
#define PANEL_APP_AREA_H    (PANEL_HEIGHT - PANEL_STATUS_BAR_H - PANEL_NAV_BAR_H)
void panel_hal_read_touch_raw(int *x, int *y, bool *pressed);
#endif
