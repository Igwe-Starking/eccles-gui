# Getting started

You need three things: a **band buffer**, a **flush function**, and a loop that feeds touch and time. `examples/host/quickstart.c` is this page as a runnable program.

## 1. Add the files

Copy `src/eccles_gui.h`, `src/eccles_gui.c` and (optionally) `config/eccles_gui_config.h` into your project, or add the repository as a component / library:

- **ESP-IDF**: put the folder in `components/eccles_gui/` (the root `CMakeLists.txt` registers it), or use `idf_component.yml`.
- **PlatformIO**: `lib_deps = https://github.com/igwe-starking/eccles-gui` (`library.json` is included).
- **Arduino**: install as a library. The config file is searched for with `__has_include`; if your build does not see `config/`, put the options in `build_flags` (`-DECCLES_GUI_NO_3D`) instead.
- **Plain CMake**: `add_subdirectory(eccles-gui)` and link `eccles::gui`.

## 2. The descriptor

```c
static uint16_t band[320 * 16];
static eccles_gui_t gui;

eccles_gui_desc_t d = {0};
d.width = 320; d.height = 480;       // the panel
d.buf = band; d.buf_pixels = 320 * 16;
d.flush = my_flush;
eccles_gui_init(&gui, &d);
```

`buf_pixels` is a number of **pixels** (not bytes) and must be at least `width`. A bigger buffer means fewer, larger flushes (faster); a smaller one saves RAM. The engine adapts: for a dirty rectangle `w` pixels wide it uses `buf_pixels / w` lines per band.

## 3. The flush callback

```c
static void my_flush(void *user, int x, int y, int w, int h, const uint16_t *px) {
    // set the panel's address window to x,y,w,h and send w*h rgb565 pixels, row after row
}
```

If your driver wants the high byte first (most SPI panels do), either swap in the driver or set `ECCLES_GUI_DESC_SWAP_BYTES` in `d.flags` (or `-DECCLES_GUI_SWAP_BYTES=1`). For DMA see [Porting](Porting.md).

## 4. Build the UI

Every widget is created under a parent; `eccles_gui_root()` is the full panel. Positions are relative to the parent. All setters redraw only what they changed.

```c
eccles_gui_obj_t root = eccles_gui_root(&gui);
eccles_gui_obj_t card = eccles_gui_panel_create(&gui, root);
eccles_gui_obj_set_style(&gui, card, ECCLES_GUI_STYLE_CARD);
eccles_gui_obj_set_area(&gui, card, 10, 10, 300, 120);

eccles_gui_obj_t sw = eccles_gui_switch_create(&gui, card, true);
eccles_gui_obj_set_align(&gui, sw, ECCLES_GUI_ALIGN_TOP_RIGHT, -10, 10);   // follows the parent's size
```

Create calls return `ECCLES_GUI_NO_OBJ` when a pool is full; `eccles_gui_last_error_str(&gui)` says which one.

**Text is not copied.** `eccles_gui_set_text(gui, obj, "literal")` stores the pointer. For text that changes, keep a buffer, change it in place, and call `eccles_gui_set_text` again with the same pointer to redraw.

## 5. The loop

```c
for (;;) {
    int x, y; bool down;
    read_touch(&x, &y, &down);
    eccles_gui_touch(&gui, x, y, down);          // hit test, events, pressed look
    uint32_t sent = eccles_gui_update(&gui, millis());   // animations, long press, fling, then redraw
    sleep_ms(sent ? 8 : 30);                     // nothing changed: sleep longer, the CPU and the bus rest
}
```

`eccles_gui_update` returns the number of pixels it flushed, so `0` means the UI is idle.

## Events

```c
static void on_event(eccles_gui_t *g, const eccles_gui_event_t *ev) {
    switch (ev->code) {
    case ECCLES_GUI_EV_CLICKED:       ...  // ev->value = list row for lists
    case ECCLES_GUI_EV_VALUE_CHANGED: ...  // slider moved / switch toggled / list selection; ev->value
    case ECCLES_GUI_EV_LONG_PRESSED:  ...
    case ECCLES_GUI_EV_DRAG:          ...  // ev->dx, ev->dy
    case ECCLES_GUI_EV_GESTURE:       ...  // ev->value = ECCLES_GUI_DIR_LEFT/RIGHT/UP/DOWN
    }
}
eccles_gui_obj_on(&gui, obj, on_event, my_pointer);   // my_pointer comes back as ev->user
```

Gestures bubble up to the first ancestor that has a callback, so a swipe on a label reaches the panel that listens. Widgets can be deleted from inside their own callback.

## No widgets at all (immediate mode)

If you only want fast drawing, define `ECCLES_GUI_NO_WIDGETS` and draw with your own callback. The engine does the banding and flushing:

```c
static void draw(eccles_gui_dc_t *dc, const eccles_gui_area_t *area, void *user) {
    eccles_gui_draw_fill(dc, 0, 0, 320, 480, 0x0000, 255);   // you must paint every pixel of the area
    eccles_gui_draw_arc(dc, 160, 240, 100, 12, 135, 405, true, 0x07E0, 255);
}
eccles_gui_render_area(&gui, 0, 0, 320, 480, draw, NULL);    // whole panel, or any rectangle
```
