// eccles_gui_port_phone.h - glue between eccles_gui and the eccles phone's panel_hal
//
//   eccles_phone_gui_init(&gui)      once, from the system ui task: gives the engine its band buffers and the flush callback
//   eccles_phone_gui_poll(&gui, ms)  every ~16 ms: reads the raw touch, feeds it to the engine, runs animations and redraws
#ifndef ECCLES_GUI_PORT_PHONE_H
#define ECCLES_GUI_PORT_PHONE_H
#include "eccles_gui.h"
#ifdef __cplusplus
extern "C" {
#endif

// band height in panel lines. 750 px * 16 lines * 2 bytes = 24 KB per buffer. smaller = less ram, more flushes
#ifndef ECCLES_PHONE_BAND_LINES
  #define ECCLES_PHONE_BAND_LINES 16
#endif
// 1 = two band buffers (48 KB): the engine draws one while the panel driver sends the other. needs ECCLES_PHONE_GUI_ASYNC
// and panel_hal_blit_raw_wait() to pay off, otherwise it only wastes ram
#ifndef ECCLES_PHONE_DOUBLE_BUFFER
  #define ECCLES_PHONE_DOUBLE_BUFFER 0
#endif

// returns false if the engine or the buffers could not be set up
bool eccles_phone_gui_init(eccles_gui_t *gui);
// call from the same task that built the ui. returns the pixels sent this call (0 = nothing changed, you can sleep longer)
uint32_t eccles_phone_gui_poll(eccles_gui_t *gui, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
#endif
