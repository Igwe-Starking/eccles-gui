# Architecture

## The band loop

```
eccles_gui_update(now)
  ├─ animations step, long press / fling tick
  └─ eccles_gui_render()
       for each dirty rectangle R (already merged, never overlapping):
         lines = buf_pixels / R.width
         for each band of `lines` rows inside R:
            dc = { buf, clip = band }
            draw the widget tree into dc           (everything is clipped to the band)
            flush(x, y, w, h, buf)                 (or the async pair, see Porting)
```

Everything that draws takes a **draw context** (`eccles_gui_dc_t`): a pointer to the band's pixels, the band's position on the panel and a clip rectangle. All coordinates in the API are panel coordinates; the context translates. This is why custom widgets and the 3D view can draw anywhere without knowing about bands.

### Consequences you can rely on

1. **The picture does not depend on the band height.** There is no state that crosses bands, so 1 line and full height give bit identical results. This is tested at 9 heights.
2. **Every band is painted from scratch.** The root widget always covers the whole panel with an opaque fill (no rounded corners, no shadow), so a band never contains stale pixels. In immediate mode *you* must paint every pixel of the area you were given.
3. **Draw order is painter's order.** A widget draws its own box, then its children in creation order (use `to_front` / `to_back`). Children are clipped to the parent's rectangle.

## Dirty rectangles

`eccles_gui_invalidate()` and every setter add a rectangle. A new one is merged with each existing one it overlaps, or that costs at most 25% extra area to merge. When the list (`ECCLES_GUI_MAX_DIRTY`, default 8) is full, the new rectangle is merged with the one that grows least. The merge guarantees the list never contains overlapping rectangles, so no pixel is drawn twice in a pass.

Setters invalidate the old *and* the new area (and the shadow margin of the style). `eccles_gui_render` copies the list and clears it **before** drawing, so a callback may invalidate for the next pass.

## Widget tree

A widget is 40 bytes (32-bit): position and size relative to its parent, `parent / first child / next sibling` as 16-bit indices, a type, flags, a style index, opacity, alignment, a small union with the per-type data, a callback and a user pointer. The pool is a fixed array; handles are indices, a freed slot is reused. Alignment is resolved when needed (position = parent size + your offset), so resizing a parent moves aligned children for free.

## Fixed point everywhere

| What | Format |
|---|---|
| Colors | RGB565, blended 3 channels at once with the "spread" trick (`0x07E0F81F`), 32 alpha levels |
| Coverage of rounded shapes | first order signed distance `(r² - d²) / 2r`, in half pixels: no sqrt, symmetric |
| 3D world | Q12 (4096 = 1.0), vertices `int16` Q12 |
| Rotation matrices | Q14 |
| Sine / cosine | Q15, quarter wave table (257 entries) with linear interpolation, 16 bit binary angles |
| Triangle edges | 16.16, evaluated from the upper vertex with one canonical formula, so shared edges never crack |

## Where the time goes, and how to trade it

- **Fill and blend** dominate. `ECCLES_GUI_EXEC_IRAM` puts those loops in internal RAM on chips where flash/PSRAM cache misses hurt.
- **Shadows** are the most expensive style attribute (several translucent rounded rects). `ECCLES_GUI_NO_SHADOW` removes them.
- **AA** costs only on rounded edges and lines; `ECCLES_GUI_NO_AA` makes them hard-edged.
- **Band height**: the 3D projection is cached per frame, but each band re-does triangle setup for the faces that touch it. Bigger bands = less repeated setup.
- **Redraw less**: the cheapest pixel is the one not drawn. A gauge that changes redraws only the gauge.
