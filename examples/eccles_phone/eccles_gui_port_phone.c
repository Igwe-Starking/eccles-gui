// eccles_gui_port_phone.c - see the header. inside the eccles phone project this goes into components/sysui (or a new
// component that REQUIRES hal eccles_gui)
#include "eccles_gui_port_phone.h"
#include "panel_hal.h"
#include "panel_hal_raw.h"
#include <string.h>

#if defined(ESP_PLATFORM)
  #include "esp_heap_caps.h"
  // band buffers must be internal, DMA capable ram: psram is too slow to render into and a panel DMA cannot always read it
  #define BAND_ALLOC(bytes) heap_caps_malloc((bytes), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
#else
  #include <stdlib.h>
  #define BAND_ALLOC(bytes) malloc(bytes)
#endif

#define BAND_PIXELS ((size_t)PANEL_WIDTH * ECCLES_PHONE_BAND_LINES)

static void port_flush(void *user, int x, int y, int w, int h, const uint16_t *pixels) {
    (void)user;
    panel_hal_blit_raw(x, y, w, h, pixels);
}

#ifdef ECCLES_PHONE_GUI_ASYNC
static void port_wait(void *user) { (void)user; panel_hal_blit_raw_wait(); }
#endif

bool eccles_phone_gui_init(eccles_gui_t *gui) {
    uint16_t *b1 = (uint16_t *)BAND_ALLOC(BAND_PIXELS * 2u);
    uint16_t *b2 = ECCLES_PHONE_DOUBLE_BUFFER ? (uint16_t *)BAND_ALLOC(BAND_PIXELS * 2u) : NULL;
    if (!b1 || (ECCLES_PHONE_DOUBLE_BUFFER && !b2)) return false;
    eccles_gui_desc_t d;
    memset(&d, 0, sizeof d);
    d.width = PANEL_WIDTH;
    d.height = PANEL_HEIGHT;
    d.buf = b1;
    d.buf2 = b2;
    d.buf_pixels = BAND_PIXELS;
    d.flush = port_flush;
#ifdef ECCLES_PHONE_GUI_ASYNC
    d.flush_wait = port_wait;
    d.flags |= ECCLES_GUI_DESC_ASYNC_FLUSH;
#endif
    return eccles_gui_init(gui, &d) == ECCLES_GUI_SUCCESSFUL;
}

uint32_t eccles_phone_gui_poll(eccles_gui_t *gui, uint32_t now_ms) {
    int x = 0, y = 0;
    bool pressed = false;
    panel_hal_read_touch_raw(&x, &y, &pressed);  // raw: the status bar and nav bar are ours
    eccles_gui_touch(gui, x, y, pressed);
    return eccles_gui_update(gui, now_ms);
}
