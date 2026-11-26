// panel_hal_raw_stub.c - drop next to panel_hal_stub.c (add it to components/hal/CMakeLists.txt SRCS) until your real
// panel driver implements panel_hal_blit_raw().
#include "panel_hal_raw.h"
#include "esp_log.h"

void panel_hal_blit_raw(int x, int y, int w, int h, const uint16_t *pixels) {
    (void)pixels;
    ESP_LOGD("panel_stub", "blit raw (%d,%d %dx%d)", x, y, w, h);
}

void panel_hal_blit_raw_wait(void) {}
