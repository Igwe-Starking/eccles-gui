# Changelog

## 1.0.0

First release.

- **Band renderer**: no framebuffer. A widget tree and up to `ECCLES_GUI_MAX_DIRTY` dirty rectangles are redrawn a few lines at a time into a buffer you provide (one or two, optional async/DMA flush with `flush_wait`).
- **2D drawing**: anti aliased rounded rectangles, discs, rings and arcs (no square roots), thick anti aliased lines, gradients, soft shadows, bevels, flat triangles, UTF-8 text (built in 5x8 font at any integer scale, 1/4/8 bpp fonts of your own), images (RGB565, color key, A8, A4, A1).
- **Widgets**: panel, label, button, image, slider, switch, checkbox, progress, arc gauge, scrolling list with kinetic scroll, custom, view3d. Styles, theme (dark/light palettes), alignment, row/column layout, per widget opacity.
- **Input**: one finger touch with click, long press, drag, swipe gestures (bubbling), slider drag, list scroll and fling.
- **Animation**: pool of tweens (position, size, opacity, value, 3D rotation, or any integer through a callback), 7 easing curves, delay, repeat, yoyo.
- **3D**: fixed point (Q12 world, Q15 sine, 16 bit angles) software rasteriser with flat, smooth (Gouraud), textured (affine), unlit and wireframe modes, painter's algorithm or optional per band z buffer, back face culling, near plane rejection. Six built in meshes. A shared per frame vertex/face cache, so the projection runs once per frame and every band reuses it.
- **Config**: `config/eccles_gui_config.h`; every pool size and feature can be removed (`ECCLES_GUI_NO_3D`, `_NO_WIDGETS`, `_NO_ANIM`, `_NO_AA`, `_NO_SHADOW`, ...).
- **Tools**: `png2eccles.py`, `ttf2eccles_font.py`, `obj2mesh.py`, `gen_data.py`, `gen_api_doc.py`, `gen_keywords.py`.
- **Tests**: 53,871 checks per configuration including render-is-independent-of-band-height, incremental-update-equals-full-redraw, async-flush buffer ownership and a seeded widget fuzzer; 12 configurations under ASan + UBSan.
- **Eccles Phone example**: port layer for `panel_hal` and a 750x1334 system UI (status bar, nav bar, home, swipe-down shade, 3D card) with a host simulator.
