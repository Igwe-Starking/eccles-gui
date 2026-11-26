// panel_hal_raw.h - the one function the eccles_gui port needs that panel_hal.h does not have yet.
//
// panel_hal_blit() draws in APP coordinates and is clipped to the app area on purpose (a sandboxed app must never reach the
// status bar). the system ui, which owns the whole panel, needs to write anywhere. add this to panel_hal.h and implement it
// in your panel driver next to panel_hal_blit() (panel_hal_raw_stub.c shows the stub). apps must NOT get this function:
// do not expose it through hostapi.
#ifndef ECCLES_PHONE_PANEL_HAL_RAW_H
#define ECCLES_PHONE_PANEL_HAL_RAW_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// blit an RGB565 buffer of w*h pixels at (x,y) in RAW full-panel coordinates (0,0 = top left corner of the glass).
// the buffer is only valid until the function returns, unless you also implement the optional async pair below.
void panel_hal_blit_raw(int x, int y, int w, int h, const uint16_t *pixels);

// OPTIONAL (define ECCLES_PHONE_GUI_ASYNC in eccles_gui_port_phone.c): if panel_hal_blit_raw() starts a DMA transfer and
// returns early, implement this to block until the last transfer has finished. the engine then draws the next band into the
// other buffer while the first one is being sent.
void panel_hal_blit_raw_wait(void);

#ifdef __cplusplus
}
#endif
#endif
