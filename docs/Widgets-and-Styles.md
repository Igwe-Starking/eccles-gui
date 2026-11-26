# Widgets and styles

## Styles

A style is 18 bytes in a table of `ECCLES_GUI_MAX_STYLES` slots; a widget stores a slot number.

```c
typedef struct {
    eccles_gui_color_t bg, bg2, border, text, accent;
    uint8_t border_w, radius, pad, shadow, opa, font, grad;
    int8_t bevel;
} eccles_gui_style_t;
```

| Field | Meaning |
|---|---|
| `bg`, `bg2` | fill and gradient end (`grad` = `ECCLES_GUI_GRAD_V` / `_H`); `bg2` is also the slider/switch/progress/arc **track** and the list separator |
| `accent` | slider fill and knob, switch on, checkbox, list selection, arc value |
| `text` | text and the slider knob center |
| `border_w`, `radius` | outline thickness, corner radius (clamped to half the smaller side) |
| `shadow` | soft shadow size in px, drawn below the widget |
| `opa` | 255 = solid fill, 0 = no fill (the border is still drawn) |
| `bevel` | +n raised, -n sunken: the classic 3D button look, n pixels |
| `font`, `pad` | font id and inner padding |

The built in slots (`ECCLES_GUI_STYLE_SCREEN, PANEL, CARD, BUTTON, BUTTON_PRIMARY, LABEL, LABEL_TITLE, LABEL_MUTED`) come from a palette:

```c
eccles_gui_theme_apply(&gui, &eccles_gui_palette_light);    // or _dark, or your own eccles_gui_palette_t
```

The theme scales radii, padding and fonts with the panel width (under 300 px: 1x text; under 600: 2x; above: 3x). Use `eccles_gui_style_add` for your own slots (it hands out slots from `ECCLES_GUI_STYLE_USER`), and change a built in one with `eccles_gui_style_set` (redraws the whole panel).

## Fonts

Font ids 0..3 are the built in 5x8 font at 1x..4x. Register more:

```c
int big = eccles_gui_font_register(&gui, &eccles_gui_font_5x8, 8);   // the same font, 8x: 40 x 72 px letters
int ui  = eccles_gui_font_register(&gui, &font_ui24, 1);             // a font from tools/ttf2eccles_font.py
style.font = (uint8_t)ui;
```

Text is UTF-8. A codepoint the font does not have is drawn as `?`.

## Widgets

| Create | Notes |
|---|---|
| `panel_create` | container; draws its style box (no fill with `STYLE_PANEL`) |
| `label_create(text)` | sizes itself to its text until you set a size; modes: left/center/right, clip/ellipsis/wrap |
| `button_create(text)` | pressed look, `CLICKED` |
| `image_create(img)` | centered; A* images are painted in the style's text color |
| `slider_create(min,max,v)` | horizontal if wider than tall, else vertical; `VALUE_CHANGED` |
| `switch_create(on)`, `checkbox_create(text,on)` | toggle on click, `VALUE_CHANGED` |
| `progress_create(min,max,v)` | horizontal or vertical |
| `arc_create(min,max,v)` | gauge, default sweep 135..405 degrees, `arc_set_thickness` |
| `list_create(items,count)` | only visible rows are drawn, so it can be long; tap selects, drag scrolls, flings |
| `custom_create(fn, user)` | `fn(dc, area, user)` draws anything |
| `view3d_create` | see [3D](3D.md) |

A change made from code (`set_value`, `set_checked`) does not send `VALUE_CHANGED`; a change made by touch does.

## Layout

```c
eccles_gui_obj_layout(&gui, row_panel, ECCLES_GUI_LAYOUT_ROW, /*pad*/ 8, /*gap*/ 6);
```

Lays the children out in a row or column. A child whose size on the main axis is 0 shares the remaining space (rounding leftovers go to the first ones, so nothing is lost); on the cross axis children are centered, or stretched if their size is 0. It runs once when you call it; it does not re-run on resize.

## Animation

```c
eccles_gui_anim_t a = {0};
a.obj = panel; a.prop = ECCLES_GUI_ANIM_Y; a.from = -200; a.to = 0;
a.duration_ms = 300; a.ease = ECCLES_GUI_EASE_OUT_BACK;
eccles_gui_anim_start(&gui, &a);          // needs eccles_gui_update() running
```

Properties: `X Y W H OPA VALUE V3D_YAW V3D_PITCH V3D_ROLL V3D_SCALE CUSTOM`. `CUSTOM` calls `exec(gui, target, value)` every frame, which is how you animate anything. Starting an animation on the same target and property replaces the old one. Deleting a widget stops its animations.

## Opacity

`eccles_gui_obj_set_opa` multiplies with the style opacity and the parent's. It is applied to each thing the widget and its children draw, which is cheap and good for fades; it is not group opacity, so overlapping children show through each other.
