# Eccles Phone integration

`examples/eccles_phone/` contains everything to put the engine on the Eccles Phone: a port layer for `panel_hal`, a complete system UI, and a host simulator that runs the same code at the real 750 x 1334.

```
make phone        # builds the simulator, writes build/phone.ppm (home) and phone_shade.ppm (shade open), converts to png
```

It was built and run against the project's **real** `components/hal/include/panel_hal.h` (the output is byte identical to the build with the trimmed header in `host_stubs/`). What was **not** possible here: building the ESP-IDF project itself (no ESP-IDF in the environment) or running on the chip, so the `#ifdef ESP_PLATFORM` allocation path in the port is untested.

## What the phone needs that it does not have yet: a raw blit

`panel_hal_blit()` draws in **app** coordinates and is clipped to the app area, on purpose: a sandboxed app must never reach the status bar. The system UI owns the whole glass, so it needs a blit in raw panel coordinates. The port declares it in `panel_hal_raw.h`:

```c
void panel_hal_blit_raw(int x, int y, int w, int h, const uint16_t *pixels);
void panel_hal_blit_raw_wait(void);     // optional, for async DMA
```

Add the declaration to `panel_hal.h`, implement it next to `panel_hal_blit()` in your panel driver (`panel_hal_raw_stub.c` is the stub), and **do not expose it through `hostapi`**: apps must not get it.

## Steps

1. Copy this repository to `components/eccles_gui/` (the root `CMakeLists.txt` registers it as an ESP-IDF component; the `config/` folder is on its include path).
2. Add `panel_hal_raw.h` to `components/hal/include/`, add `panel_hal_raw_stub.c` to `components/hal/CMakeLists.txt` `SRCS` (or implement it in your driver).
3. Copy `eccles_gui_port_phone.[ch]`, `eccles_phone_ui.[ch]` into `components/sysui/` and add them to its `CMakeLists.txt` with `REQUIRES hal eccles_gui`.
4. In the system UI task:

```c
static eccles_gui_t gui;                  // ~7 KB static (DRAM)

void sysui_task(void *arg) {
    eccles_phone_gui_init(&gui);          // allocates a 24 KB internal DMA band buffer, initialises the engine
    eccles_phone_ui_hooks_t hooks = { .on_launch = launch_app, .on_back = ui_back, .on_home = ui_home, .on_recents = ui_recents };
    eccles_phone_ui_build(&gui, &hooks);
    for (;;) {
        uint32_t sent = eccles_phone_gui_poll(&gui, (uint32_t)(esp_timer_get_time() / 1000));   // touch + animations + redraw
        vTaskDelay(pdMS_TO_TICKS(sent ? 8 : 30));
    }
}
```

   Give the task **6 KB of stack** (the engine's worst frame is under 1 KB; the rest is margin for your callbacks).

5. Settings for the phone build (`sdkconfig.defaults` / `eccles_gui_config.h`): `ECCLES_GUI_EXEC_IRAM 1` (code runs from flash through a cache, so keeping the per-pixel loops in IRAM is the first speed experiment to try; measure it), and `ECCLES_GUI_MAX_WIDGETS` at the default 48 is enough for the example screen (it uses 42).

## What the example UI replaces in the project today

| Existing placeholder call | In the engine UI |
|---|---|
| `panel_hal_draw_status_bar(has_notification)` | the status bar panel with clock, battery and the notification dot (`eccles_phone_ui_set_notification_dot`) |
| `panel_hal_draw_nav_bar()` | three clickable custom widgets: back, home, recents (hooks `on_back / on_home / on_recents`) |
| `panel_hal_draw_overlay(color, h)` | any widget: the notification shade is a card that slides in with an animation (`ECCLES_GUI_EASE_OUT_BACK`), a swipe down on the status bar opens it, a swipe up closes it |
| `panel_hal_show_call_screen(number, incoming)` | build a panel with labels and two buttons; the example's shade shows the pattern |
| `panel_hal_read_touch_raw` | used as is by `eccles_phone_gui_poll`; sandboxed apps keep using `panel_hal_read_touch` |

The apps themselves still draw into their own area through `panel_hal_blit`; the engine owns the chrome and overlays. (A natural next step, not included: expose a small `hostapi` binding so WASM apps can use the widget set instead of pushing raw pixels.)

## Memory on the phone

| | |
|---|---|
| Engine context | about 7 KB static |
| Band buffer, 16 lines | 24,000 bytes internal DMA RAM |
| Band buffer, second buffer | +24,000 bytes only with `ECCLES_PHONE_DOUBLE_BUFFER 1` *and* an async `panel_hal_blit_raw` (`ECCLES_PHONE_GUI_ASYNC`) |
| A full framebuffer, for comparison | 2,001,000 bytes |
| Full-screen frame | 1,000,500 pixels through 84 flushes; an idle frame with the 3D card spinning is 203,796 pixels (the card only) |

## The example, piece by piece

- **Chrome**: status bar and nav bar are created last, so they are on top of the shade.
- **Home**: clock in the built in font at 8x (`eccles_gui_font_register(&gui, &eccles_gui_font_5x8, 8)`), cards, slider, progress, a 3x2 app grid made of buttons with their own styles (`eccles_gui_style_add`).
- **3D card**: a `view3d` with a cube, a sphere and a torus. The sphere and torus spin through two endless `ECCLES_GUI_ANIM_V3D_YAW` animations; dragging the card turns the cube (`ECCLES_GUI_EV_DRAG`).
- **Shade**: a custom animation drives its y; a transparent "dim" panel behind it closes it when tapped.
