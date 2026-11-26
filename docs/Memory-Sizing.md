# Memory sizing

Nothing is allocated at run time. The RAM cost is `sizeof(eccles_gui_t)` (a static you declare) plus the band buffer(s) you hand over.

## The context

Default configuration, **32-bit pointers** (measured by compiling the struct layout with `-mx32`; a 64-bit host is 7,880 bytes):

| Pool | Count (macro) | Bytes each | Total |
|---|---|---|---|
| Widgets | `48` | 40 | 1,920 |
| Styles | `16` | 18 | 288 |
| Font slots | `8` | 8 | 64 |
| Dirty rectangles | `8` | 8 | 64 |
| Animations | `8` | 44 | 352 |
| view3d scenes | `2` | 176 | 352 |
| 3D vertex cache | `192` | 8 | 1,536 |
| 3D face cache | `256` | 8 | 2,048 |
| Everything else (descriptor, stats, input state, 3D cache header) | | | 172 |
| **`sizeof(eccles_gui_t)`, measured** | | | **6,796** |

The 3D part (`views` + vertex/face cache) is **3,936 bytes of the 6,796**. `ECCLES_GUI_NO_3D` removes all of it; or shrink it: a scene of a cube and a sphere needs only `ECCLES_GUI_3D_MAX_VERTS 80` and `_MAX_FACES 100` (24+42 = 66 vertices, 12+80 = 92 faces), which saves about 2.1 KB.

Rules of thumb for sizing the pools:

- **Widgets**: count them in your busiest screen, add the ones a transition creates before it deletes the old ones. A list or a scroll area costs one widget no matter how many rows it shows.
- **Dirty rectangles**: 8 is plenty; the list merges when full, so too few costs speed (bigger redraws), never correctness.
- **3D**: vertices and faces are counted over *all* objects of the visible scene. Built in meshes: cube 24 v / 12 f, pyramid 16 / 6, cylinder 68 / 64, sphere 42 / 80, torus 96 / 192, card 25 / 32. Faces that do not fit are dropped and reported in `eccles_gui_get_stats().faces_dropped`; backfacing and off-screen faces do not use the cache.

## The band buffer

`buf_pixels x 2` bytes. This is the one number you tune.

| Panel | Lines | Buffer | Flushes for a full redraw |
|---|---|---|---|
| 240 x 320 | 16 | 7.5 KB | 20 |
| 320 x 480 | 16 | 10 KB | 30 |
| 480 x 800 | 16 | 15 KB | 50 |
| 750 x 1334 (Eccles Phone) | 16 | 24 KB | 84 |
| 750 x 1334 | 8 | 12 KB | 167 |
| 750 x 1334 | 1 | 1.5 KB | 1334 |

Bigger bands mean fewer SPI/DMA transactions and less repeated triangle setup; smaller ones save RAM. A band of 1 line works (and is tested), it is just slow. For small redraws (a gauge, a switch) the band is as many lines as fit, so a narrow rectangle gets tall bands.

Put band buffers in **internal, DMA capable RAM** (`MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL` on ESP32): rendering into PSRAM is slow and a panel DMA often cannot read it.

## z buffer

`desc.zbuf` is optional and has the **same size as the band buffer** (`buf_pixels` entries of 16 bits). It is only worth it for scenes where objects interpenetrate or where painter's sorting is wrong; single convex objects and well separated ones do not need it.

## Stack

Measured on x86-64: the worst frame is the 3D draw at about 800 bytes; each level of widget nesting adds about 320 bytes (the draw is recursive over the tree). Give the UI task **4 KB** (6 KB if you nest deeply or use ESP-IDF logging in callbacks). On 32-bit chips frames are smaller.

## Flash

x86-64, `-Os`, function sections (what you save on Thumb-2 / Xtensa is typically 20-35%, but measure with `make size` on your toolchain):

| Configuration | Code + data |
|---|---|
| Everything | 56 KB |
| `ECCLES_GUI_NO_3D` | 40 KB |
| `NO_3D + NO_ANIM + NO_SHADOW + NO_AA` | 38 KB |
| `NO_WIDGETS` (renderer, drawing, text, 3D) | 33 KB |

Data inside those numbers: the built in font 2.0 KB, the sine table 0.5 KB, the six meshes 6.2 KB (8.7 KB together, measured) (`ECCLES_GUI_NO_BUILTIN_MESHES`), all `const` so they live in flash.
