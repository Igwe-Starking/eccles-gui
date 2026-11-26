# Porting

You provide **one function**. This page shows the usual shapes. The snippets below are reference code written against the public APIs of those drivers; they were **not compiled in this repository** (only the portable engine and the host examples are), so treat them as a starting point.

## The contract

```c
typedef void (*eccles_gui_flush_fn)(void *user, int x, int y, int w, int h, const uint16_t *pixels);
```

`pixels` is `w x h` RGB565, row after row, no padding. The buffer is valid until `flush` returns (sync) or until `flush_wait` returns (async); never keep the pointer.

The engine has no HAL: no clock (you pass milliseconds to `eccles_gui_update`), no touch driver (you pass coordinates to `eccles_gui_touch`), no heap, no OS calls.

## Synchronous flush (simplest)

```c
static void flush(void *u, int x, int y, int w, int h, const uint16_t *px) {
    lcd_set_window(x, y, x + w - 1, y + h - 1);
    lcd_write_pixels(px, (size_t)w * h);       // blocks until sent
}
```

## ESP-IDF `esp_lcd` (SPI / RGB / i80)

```c
static void flush(void *u, int x, int y, int w, int h, const uint16_t *px) {
    esp_lcd_panel_draw_bitmap((esp_lcd_panel_handle_t)u, x, y, x + w, y + h, px);   // x_end/y_end are exclusive
}
```

For DMA, make the transfer asynchronous and tell the engine:

```c
static SemaphoreHandle_t done;
static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *u) {
    BaseType_t hp = pdFALSE; xSemaphoreGiveFromISR(done, &hp); return hp == pdTRUE;
}
static void wait(void *u) { xSemaphoreTake(done, portMAX_DELAY); xSemaphoreGive(done); }   // block until the last transfer ended

eccles_gui_desc_t d = {0};
d.buf = buf_a; d.buf2 = buf_b; d.buf_pixels = W * 16;   // both in MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL
d.flush = flush; d.flush_wait = wait;
d.flags = ECCLES_GUI_DESC_ASYNC_FLUSH | ECCLES_GUI_DESC_SWAP_BYTES;
```

With two buffers the engine renders band N+1 while band N is on the wire, and calls `flush_wait` before it reuses a buffer and when a render pass ends. (The test suite simulates exactly this with a fake DMA and fails if a buffer is touched before `flush_wait` returns.) Byte swapping must happen in exactly one place: here (`ECCLES_GUI_DESC_SWAP_BYTES`) or in your panel driver, not both.

## TFT_eSPI / Adafruit_GFX (Arduino)

```c
static void flush(void *u, int x, int y, int w, int h, const uint16_t *px) {
    tft.startWrite();
    tft.setAddrWindow(x, y, w, h);
    tft.pushPixels((uint16_t *)px, (uint32_t)w * h);   // TFT_eSPI; Adafruit: tft.writePixels((uint16_t *)px, w * h, true)
    tft.endWrite();
}
```

Most SPI panels want the high byte of each pixel first. Swap in exactly one place: either set `ECCLES_GUI_DESC_SWAP_BYTES`, or let your display library do it (check what its `pushPixels` / `setSwapBytes` does with the data you give it).

## STM32 HAL (FSMC/LTDC/SPI+DMA)

Same shape: set the window, start `HAL_SPI_Transmit_DMA` or copy into the LTDC layer in the flush; implement `flush_wait` with the DMA-complete flag. If the buffer is in cached SRAM, clean the D-cache range of the band before starting the DMA.

## Time and touch

```c
uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);   // or millis(), HAL_GetTick(), k_uptime_get_32()
eccles_gui_touch(&gui, x, y, pressed);                    // every poll, with the finger state
eccles_gui_update(&gui, now);
```

The clock may wrap (tested). If you poll touch from an interrupt, store the last sample and feed it from the UI task: the engine is not thread safe.

## IRAM

`#define ECCLES_GUI_EXEC_IRAM 1` puts the pixel loops in internal RAM (a few KB) for ESP32 (`IRAM_ATTR`), RP2040 (`.time_critical`), STM32 (`.RamFunc`) and Zephyr (`__ramfunc`). For other chips define `ECCLES_GUI_IRAM_ATTR` yourself. It is worth trying first when the UI is your slowest task; measure it on your board.

## Threads

One task owns the context. If another task wants to change the UI (a battery level, a notification), post a message to the UI task and have it call the setters, or wrap *all* calls (including `eccles_gui_update`) in one mutex.

## Checklist when the picture is wrong

| Symptom | Likely cause |
|---|---|
| Colors are swapped red/blue or look "electric" | byte order: toggle `ECCLES_GUI_DESC_SWAP_BYTES`, or RGB/BGR in the panel init |
| Image is shifted / skewed by a few pixels per row | the driver's window width does not match `w`, or the row stride assumption (`pixels` has no padding) |
| Stripes of garbage every N lines | band buffer not DMA capable, or reused before the DMA ended: use `flush_wait` |
| Touch lands in the wrong place | the panel and the touch use different axes: map in your `read_touch` before `eccles_gui_touch` |
| Everything redraws all the time | something invalidates every frame (a label whose text you set each frame). Only call setters when a value changes |
| `ECCLES_GUI_ERR_NO_MEMORY` | `eccles_gui_last_error_str()` names the pool; raise it in the config |
