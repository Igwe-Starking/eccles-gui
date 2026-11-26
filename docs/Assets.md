# Assets: icons, fonts, models

Everything is converted **offline** into `const` C arrays that stay in flash. The tools need Python 3 (`ppm2png`, `png2eccles` and `ttf2eccles_font` need Pillow; `obj2mesh` and `gen_*` need nothing).

## Icons and images: `png2eccles.py`

```
python3 tools/png2eccles.py wifi.png --name icon_wifi --format a8 > icon_wifi.c
```

| Format | Bytes per pixel | Use |
|---|---|---|
| `a8` | 1 | **icons**: an 8 bit alpha mask that you paint in any color (`eccles_gui_draw_image(dc, x, y, &icon, color, opa)`, or `image_create` which uses the style's text color) |
| `a4` | 0.5 | the same, half the size |
| `a1` | 0.125 | hard edged glyph masks |
| `rgb565` | 2 | photos and full color logos (opaque) |
| `key` | 2 | rgb565 where `--key 0xF81F` (default) is transparent |

```c
extern const eccles_gui_image_t icon_wifi;
eccles_gui_obj_t img = eccles_gui_image_create(&gui, parent, &icon_wifi);
```

There is no scaling or rotation of 2D images; render them at the size you use. (To scale or rotate an image, put it on the 3D `card` mesh as a texture.)

## Fonts: `ttf2eccles_font.py`

```
python3 tools/ttf2eccles_font.py MyFont.ttf --size 28 --bpp 4 --symbols "°•€" --name font_ui28 > font_ui28.c
python3 tools/ttf2eccles_font.py MyFont.ttf --size 20 --range 0x20-0x7E --range 0xC0-0x17F --name font_latin20 > font_latin20.c
```

```c
extern const eccles_gui_font_t font_ui28;
int id = eccles_gui_font_register(&gui, &font_ui28, 1);
style.font = (uint8_t)id;
```

4 bpp (16 gray levels) is the best size/quality trade; 8 bpp is smoother and twice the size; 1 bpp is hard edged and tiny. Size is the pixel height of the em. A font of 95 ASCII glyphs at 24 px, 4 bpp is roughly 8 KB. **Check the font's license before you ship the generated file.**

## 3D models: `obj2mesh.py`

```
python3 tools/obj2mesh.py ship.obj --name ship > ship_mesh.c           # smooth shading
python3 tools/obj2mesh.py rock.obj --name rock --flat > rock_mesh.c    # faceted
```

Centers and scales the model to +-1, triangulates, computes normals, flips z (OBJ is right handed, the engine looks along +z) and reverses the winding so the model looks like it did in your modeller. It prints the vertex and face counts: the default 3D cache holds 192 / 256, raise `ECCLES_GUI_3D_MAX_VERTS / _FACES` in the config for bigger models. Smooth shading shares vertices, `--flat` gives every face its own.

```c
extern const eccles_gui_mesh_t ship;
eccles_gui_view3d_add(&gui, v, &ship, color);
```

For textured models give the OBJ `vt` coordinates and use `ECCLES_GUI_3D_TEXTURED` with a power-of-two RGB565 texture from `png2eccles.py --format rgb565` (the mapping is affine: keep triangles smallish).
