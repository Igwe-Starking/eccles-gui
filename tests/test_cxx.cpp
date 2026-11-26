//the header must be usable from C++ (extern "C", no C-only constructs in the public part)
#include "eccles_gui.h"
#include <cstdio>
#include <cstring>

static uint16_t band[100 * 8];
static void flush(void *, int, int, int, int, const uint16_t *) {}

int main() {
    static eccles_gui_t gui;
    eccles_gui_desc_t d;
    std::memset(&d, 0, sizeof d);
    d.width = 100; d.height = 60; d.buf = band; d.buf_pixels = 100 * 8; d.flush = flush;
    if (eccles_gui_init(&gui, &d) != ECCLES_GUI_SUCCESSFUL) return 1;
    eccles_gui_obj_t b = eccles_gui_button_create(&gui, eccles_gui_root(&gui), "ok");
    eccles_gui_obj_set_pos(&gui, b, 10, 10);
    eccles_gui_area_t a = eccles_gui_area(1, 2, 3, 4);
    if (a.x2 != 4 || a.y2 != 6) return 2;
    eccles_gui_update(&gui, 0);
    std::printf("c++ ok, context %u bytes\n", (unsigned)sizeof gui);
    return 0;
}
