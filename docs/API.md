# eccles_gui API reference

Generated from the comments in `src/eccles_gui.h` by `tools/gen_api_doc.py`. Do not edit by hand.
Everything that can fail returns `eccles_gui_status_t` (`ECCLES_GUI_SUCCESSFUL`, `ECCLES_GUI_FAILED`, `ECCLES_GUI_UNSUPPORTED`); the reason is in `eccles_gui_last_error()` / `eccles_gui_last_error_str()`.

## Setup and the main loop

### `eccles_gui_init`

```c
eccles_gui_status_t eccles_gui_init(eccles_gui_t *gui, const eccles_gui_desc_t *desc);
```

Set up the context. every pool is empty afterwards (and, with widgets, there is a root widget the size of the panel) the built in theme is the dark palette. fails with ECCLES_GUI_ERR_BUFFER if buf_pixels < width, ERR_NO_FLUSH without a flush callback

### `eccles_gui_deinit`

```c
void eccles_gui_deinit(eccles_gui_t *gui);
```

Forget everything. (there is nothing to free, this just marks the context unusable)

### `eccles_gui_update`

```c
uint32_t eccles_gui_update(eccles_gui_t *gui, uint32_t now_ms);
```

The normal main loop call. now_ms is any monotonic millisecond counter (it may wrap), it drives animations, long press and kinetic scrolling. then it redraws what changed. returns the number of pixels that were flushed

### `eccles_gui_render`

```c
uint32_t eccles_gui_render(eccles_gui_t *gui);
```

Redraw every dirty rectangle now (no timekeeping). returns the number of pixels flushed

### `eccles_gui_render_area`

```c
uint32_t eccles_gui_render_area(eccles_gui_t *gui, int x, int y, int w, int h, eccles_gui_draw_fn fn, void *user);
```

Immediate mode: redraw one region of the panel with your own function, band by band, and flush it your function must paint every pixel of the area (start with eccles_gui_draw_fill). no widgets involved

### `eccles_gui_invalidate`

```c
void eccles_gui_invalidate(eccles_gui_t *gui, int x, int y, int w, int h);
```

Mark a rectangle as changed, the next render redraws it. clipped to the panel, overlapping ones are merged

### `eccles_gui_invalidate_all`

```c
void eccles_gui_invalidate_all(eccles_gui_t *gui);
```

### `eccles_gui_is_dirty`

```c
bool eccles_gui_is_dirty(const eccles_gui_t *gui);
```

### `eccles_gui_last_error`

```c
eccles_gui_error_t eccles_gui_last_error(const eccles_gui_t *gui);
```

### `eccles_gui_last_error_str`

```c
const char *eccles_gui_last_error_str(const eccles_gui_t *gui);
```

Which pool ran out (or "")

### `eccles_gui_error_name`

```c
const char *eccles_gui_error_name(eccles_gui_error_t e);
```

### `eccles_gui_get_stats`

```c
eccles_gui_stats_t eccles_gui_get_stats(const eccles_gui_t *gui);
```

## Fonts and text

### `eccles_gui_font_register`

```c
int eccles_gui_font_register(eccles_gui_t *gui, const eccles_gui_font_t *font, uint8_t scale);
```

Add a font (it stays in flash, only the pointer is kept). scale 1..8 enlarges it by that integer factor. returns the id or -1

### `eccles_gui_text_width`

```c
int eccles_gui_text_width(const eccles_gui_t *gui, uint8_t font, const char *text);
```

Width in pixels of the first line of text (up to '\n')

### `eccles_gui_text_height`

```c
int eccles_gui_text_height(const eccles_gui_t *gui, uint8_t font, const char *text, int max_w);
```

Height of the text when it is wrapped at max_w pixels (0 = no wrapping, only '\n' breaks lines)

### `eccles_gui_font_height`

```c
int eccles_gui_font_height(const eccles_gui_t *gui, uint8_t font);
```

Line height of a font in pixels

## Drawing

### `eccles_gui_draw_fill`

```c
void eccles_gui_draw_fill(eccles_gui_dc_t *dc, int x, int y, int w, int h, eccles_gui_color_t c, uint8_t opa);
```

### `eccles_gui_draw_rrect`

```c
void eccles_gui_draw_rrect(eccles_gui_dc_t *dc, int x, int y, int w, int h, int r, eccles_gui_color_t c, uint8_t opa);
```

Rounded rectangle, anti aliased. a radius of w/2 makes a pill or a disc

### `eccles_gui_draw_border`

```c
void eccles_gui_draw_border(eccles_gui_dc_t *dc, int x, int y, int w, int h, int r, int bw, eccles_gui_color_t c, uint8_t opa);
```

Outline of a rounded rectangle, bw pixels thick, drawn inside x,y,w,h

### `eccles_gui_draw_gradient`

```c
void eccles_gui_draw_gradient(eccles_gui_dc_t *dc, int x, int y, int w, int h, int r, eccles_gui_color_t c1, eccles_gui_color_t c2, uint8_t dir, uint8_t opa);
```

Rounded rectangle with a gradient (ECCLES_GUI_GRAD_V / _H)

### `eccles_gui_draw_shadow`

```c
void eccles_gui_draw_shadow(eccles_gui_dc_t *dc, int x, int y, int w, int h, int r, int size, uint8_t opa);
```

Soft shadow of a rounded rectangle (size = blur in pixels), drawn around/below it, offset down by size/3

### `eccles_gui_draw_disc`

```c
void eccles_gui_draw_disc(eccles_gui_dc_t *dc, int cx, int cy, int r, eccles_gui_color_t c, uint8_t opa);
```

### `eccles_gui_draw_line`

```c
void eccles_gui_draw_line(eccles_gui_dc_t *dc, int x0, int y0, int x1, int y1, int width, eccles_gui_color_t c, uint8_t opa);
```

Anti aliased line, width in pixels, endpoints are pixel centers

### `eccles_gui_draw_arc`

```c
void eccles_gui_draw_arc(eccles_gui_dc_t *dc, int cx, int cy, int r, int thickness, int a0, int a1, bool round_caps, eccles_gui_color_t c, uint8_t opa);
```

Ring segment from a0 to a1 degrees (0 = 3 o'clock, growing clockwise), thickness pixels inside radius r. rounded caps if round_caps

### `eccles_gui_draw_triangle`

```c
void eccles_gui_draw_triangle(eccles_gui_dc_t *dc, int x0, int y0, int x1, int y1, int x2, int y2, eccles_gui_color_t c, uint8_t opa);
```

Filled triangle (no anti aliasing)

### `eccles_gui_draw_pixel`

```c
void eccles_gui_draw_pixel(eccles_gui_dc_t *dc, int x, int y, eccles_gui_color_t c, uint8_t opa);
```

One pixel

### `eccles_gui_draw_text`

```c
int eccles_gui_draw_text(eccles_gui_dc_t *dc, int x, int y, uint8_t font, const char *text, eccles_gui_color_t c, uint8_t opa);
```

Text: utf-8, '\n' starts a new line. (x,y) is the top left. returns the width of the widest line

### `eccles_gui_draw_text_box`

```c
void eccles_gui_draw_text_box(eccles_gui_dc_t *dc, const eccles_gui_area_t *box, uint8_t font, const char *text, eccles_gui_color_t c, uint8_t opa, uint8_t mode);
```

Text in a box with alignment (ECCLES_GUI_TEXT_*), wrapping / ellipsis, top aligned

### `eccles_gui_draw_image`

```c
void eccles_gui_draw_image(eccles_gui_dc_t *dc, int x, int y, const eccles_gui_image_t *img, eccles_gui_color_t c, uint8_t opa);
```

An image. for the A* formats the image is painted in color c

## Colors and math

### `eccles_gui_color_mix`

```c
eccles_gui_color_t eccles_gui_color_mix(eccles_gui_color_t a, eccles_gui_color_t b, uint8_t t);
```

T = 0 gives a, 255 gives b

### `eccles_gui_sin`

```c
int16_t eccles_gui_sin(uint16_t angle);
```

Sine and cosine of a binary angle (65536 = 360 degrees) as Q15 (32767 = 1.0)

### `eccles_gui_cos`

```c
int16_t eccles_gui_cos(uint16_t angle);
```

## 3D drawing

### `eccles_gui_draw_view3d`

```c
void eccles_gui_draw_view3d(eccles_gui_dc_t *dc, const eccles_gui_view3d_t *scene, int x, int y, int w, int h);
```

Draws a scene (the objects of a view3d struct) into the viewport x,y,w,h of this band. works in any draw callback the geometry is projected once per frame and reused by every band, so call it with the same scene and viewport in every band

### `eccles_gui_view3d_defaults`

```c
void eccles_gui_view3d_defaults(eccles_gui_view3d_t *scene, int view_h);
```

Fill a scene with defaults: camera 4 units back, focal = 1 view height, light from the top left front, no objects

## Styles and theme

### `eccles_gui_style_set`

```c
eccles_gui_status_t eccles_gui_style_set(eccles_gui_t *gui, uint8_t slot, const eccles_gui_style_t *style);
```

Copy a style into a slot (any slot < ECCLES_GUI_MAX_STYLES). the whole panel is redrawn

### `eccles_gui_style_get`

```c
const eccles_gui_style_t *eccles_gui_style_get(const eccles_gui_t *gui, uint8_t slot);
```

### `eccles_gui_style_add`

```c
int eccles_gui_style_add(eccles_gui_t *gui, const eccles_gui_style_t *style);
```

The first free slot at or after ECCLES_GUI_STYLE_USER, filled with `style`. returns the slot or -1

### `eccles_gui_theme_apply`

```c
void eccles_gui_theme_apply(eccles_gui_t *gui, const eccles_gui_palette_t *palette);
```

Rebuild the 8 built in slots from a palette and redraw everything

## Widgets: creating

### `eccles_gui_root`

```c
eccles_gui_obj_t eccles_gui_root(const eccles_gui_t *gui);
```

The full panel widget that holds everything

### `eccles_gui_panel_create`

```c
eccles_gui_obj_t eccles_gui_panel_create(eccles_gui_t *gui, eccles_gui_obj_t parent);
```

Every create call returns ECCLES_GUI_NO_OBJ when the pool is full or the parent is bad (see eccles_gui_last_error) the new widget is on top of its siblings, at 0,0, 100x40 (labels fit their text), with the style that suits its kind

### `eccles_gui_label_create`

```c
eccles_gui_obj_t eccles_gui_label_create(eccles_gui_t *gui, eccles_gui_obj_t parent, const char *text);
```

### `eccles_gui_button_create`

```c
eccles_gui_obj_t eccles_gui_button_create(eccles_gui_t *gui, eccles_gui_obj_t parent, const char *text);
```

### `eccles_gui_image_create`

```c
eccles_gui_obj_t eccles_gui_image_create(eccles_gui_t *gui, eccles_gui_obj_t parent, const eccles_gui_image_t *img);
```

### `eccles_gui_slider_create`

```c
eccles_gui_obj_t eccles_gui_slider_create(eccles_gui_t *gui, eccles_gui_obj_t parent, int min, int max, int value);
```

### `eccles_gui_switch_create`

```c
eccles_gui_obj_t eccles_gui_switch_create(eccles_gui_t *gui, eccles_gui_obj_t parent, bool on);
```

### `eccles_gui_checkbox_create`

```c
eccles_gui_obj_t eccles_gui_checkbox_create(eccles_gui_t *gui, eccles_gui_obj_t parent, const char *text, bool checked);
```

### `eccles_gui_progress_create`

```c
eccles_gui_obj_t eccles_gui_progress_create(eccles_gui_t *gui, eccles_gui_obj_t parent, int min, int max, int value);
```

### `eccles_gui_arc_create`

```c
eccles_gui_obj_t eccles_gui_arc_create(eccles_gui_t *gui, eccles_gui_obj_t parent, int min, int max, int value);
```

### `eccles_gui_list_create`

```c
eccles_gui_obj_t eccles_gui_list_create(eccles_gui_t *gui, eccles_gui_obj_t parent, const char *const *items, int count);
```

A scrolling list of text rows, only the visible rows are drawn so it can be as long as you like. items are not copied

### `eccles_gui_custom_create`

```c
eccles_gui_obj_t eccles_gui_custom_create(eccles_gui_t *gui, eccles_gui_obj_t parent, eccles_gui_draw_fn fn, void *user);
```

### `eccles_gui_view3d_create`

```c
eccles_gui_obj_t eccles_gui_view3d_create(eccles_gui_t *gui, eccles_gui_obj_t parent);
```

A 3d scene. needs ECCLES_GUI_MAX_VIEWS3D > 0

## Widgets: the tree

### `eccles_gui_obj_delete`

```c
eccles_gui_status_t eccles_gui_obj_delete(eccles_gui_t *gui, eccles_gui_obj_t obj);
```

Delete a widget and everything inside it. the root cannot be deleted

### `eccles_gui_obj_set_parent`

```c
eccles_gui_status_t eccles_gui_obj_set_parent(eccles_gui_t *gui, eccles_gui_obj_t obj, eccles_gui_obj_t parent);
```

### `eccles_gui_obj_to_front`

```c
void eccles_gui_obj_to_front(eccles_gui_t *gui, eccles_gui_obj_t obj);
```

### `eccles_gui_obj_to_back`

```c
void eccles_gui_obj_to_back(eccles_gui_t *gui, eccles_gui_obj_t obj);
```

### `eccles_gui_obj_valid`

```c
bool eccles_gui_obj_valid(const eccles_gui_t *gui, eccles_gui_obj_t obj);
```

### `eccles_gui_obj_parent`

```c
eccles_gui_obj_t eccles_gui_obj_parent(const eccles_gui_t *gui, eccles_gui_obj_t obj);
```

### `eccles_gui_obj_child`

```c
eccles_gui_obj_t eccles_gui_obj_child(const eccles_gui_t *gui, eccles_gui_obj_t obj);
```

First child (bottom of the stack)

### `eccles_gui_obj_next`

```c
eccles_gui_obj_t eccles_gui_obj_next(const eccles_gui_t *gui, eccles_gui_obj_t obj);
```

Next sibling

### `eccles_gui_obj_at`

```c
eccles_gui_obj_t eccles_gui_obj_at(const eccles_gui_t *gui, int x, int y, bool only_clickable);
```

Topmost visible widget at a panel position (ECCLES_GUI_NO_OBJ if none). with only_clickable, skips the ones that cannot be touched

## Widgets: geometry and look

### `eccles_gui_obj_set_pos`

```c
void eccles_gui_obj_set_pos(eccles_gui_t *gui, eccles_gui_obj_t obj, int x, int y);
```

### `eccles_gui_obj_set_size`

```c
void eccles_gui_obj_set_size(eccles_gui_t *gui, eccles_gui_obj_t obj, int w, int h);
```

### `eccles_gui_obj_set_area`

```c
void eccles_gui_obj_set_area(eccles_gui_t *gui, eccles_gui_obj_t obj, int x, int y, int w, int h);
```

### `eccles_gui_obj_set_align`

```c
void eccles_gui_obj_set_align(eccles_gui_t *gui, eccles_gui_obj_t obj, eccles_gui_align_t align, int ox, int oy);
```

### `eccles_gui_obj_set_style`

```c
void eccles_gui_obj_set_style(eccles_gui_t *gui, eccles_gui_obj_t obj, uint8_t style);
```

### `eccles_gui_obj_set_opa`

```c
void eccles_gui_obj_set_opa(eccles_gui_t *gui, eccles_gui_obj_t obj, uint8_t opa);
```

0..255, multiplies with the style opa and the parent's

### `eccles_gui_obj_set_hidden`

```c
void eccles_gui_obj_set_hidden(eccles_gui_t *gui, eccles_gui_obj_t obj, bool hidden);
```

### `eccles_gui_obj_set_enabled`

```c
void eccles_gui_obj_set_enabled(eccles_gui_t *gui, eccles_gui_obj_t obj, bool enabled);
```

### `eccles_gui_obj_set_clickable`

```c
void eccles_gui_obj_set_clickable(eccles_gui_t *gui, eccles_gui_obj_t obj, bool clickable);
```

### `eccles_gui_obj_get_area`

```c
eccles_gui_area_t eccles_gui_obj_get_area(const eccles_gui_t *gui, eccles_gui_obj_t obj);
```

Absolute position and size in panel coordinates

### `eccles_gui_obj_layout`

```c
eccles_gui_status_t eccles_gui_obj_layout(eccles_gui_t *gui, eccles_gui_obj_t obj, int direction, int pad, int gap);
```

Lay the children out in a row or column inside their parent: padding at the edges, gap between them. a child with size 0 on the main axis shares the remaining space equally. on the cross axis every child is centered

### `eccles_gui_obj_set_user`

```c
void eccles_gui_obj_set_user(eccles_gui_t *gui, eccles_gui_obj_t obj, void *user);
```

### `eccles_gui_obj_get_user`

```c
void *eccles_gui_obj_get_user(const eccles_gui_t *gui, eccles_gui_obj_t obj);
```

### `eccles_gui_obj_on`

```c
void eccles_gui_obj_on(eccles_gui_t *gui, eccles_gui_obj_t obj, eccles_gui_event_fn cb, void *user);
```

One callback per widget, it gets every event of that widget

### `eccles_gui_obj_invalidate`

```c
void eccles_gui_obj_invalidate(eccles_gui_t *gui, eccles_gui_obj_t obj);
```

Redraw this widget (call after changing text you edited in place)

## Widgets: content

### `eccles_gui_set_text`

```c
eccles_gui_status_t eccles_gui_set_text(eccles_gui_t *gui, eccles_gui_obj_t obj, const char *text);
```

Label, button and checkbox. the text is NOT copied, keep it alive (a static string or your own buffer: after you change a buffer in place, call set_text again with the same pointer to redraw)

### `eccles_gui_set_text_mode`

```c
eccles_gui_status_t eccles_gui_set_text_mode(eccles_gui_t *gui, eccles_gui_obj_t obj, uint8_t mode);
```

ECCLES_GUI_TEXT_LEFT/CENTER/RIGHT | ECCLES_GUI_TEXT_CLIP/ELLIPSIS/WRAP

### `eccles_gui_image_set`

```c
eccles_gui_status_t eccles_gui_image_set(eccles_gui_t *gui, eccles_gui_obj_t obj, const eccles_gui_image_t *img);
```

### `eccles_gui_set_range`

```c
eccles_gui_status_t eccles_gui_set_range(eccles_gui_t *gui, eccles_gui_obj_t obj, int min, int max);
```

Slider / progress / arc range and value. a change from code does not send VALUE_CHANGED (a change by touch does)

### `eccles_gui_set_value`

```c
eccles_gui_status_t eccles_gui_set_value(eccles_gui_t *gui, eccles_gui_obj_t obj, int value);
```

### `eccles_gui_get_value`

```c
int eccles_gui_get_value(const eccles_gui_t *gui, eccles_gui_obj_t obj);
```

Slider/progress/arc value, switch/checkbox 0 or 1, list selected row (-1 = none)

### `eccles_gui_set_checked`

```c
eccles_gui_status_t eccles_gui_set_checked(eccles_gui_t *gui, eccles_gui_obj_t obj, bool checked);
```

### `eccles_gui_is_checked`

```c
bool eccles_gui_is_checked(const eccles_gui_t *gui, eccles_gui_obj_t obj);
```

### `eccles_gui_arc_set_angles`

```c
eccles_gui_status_t eccles_gui_arc_set_angles(eccles_gui_t *gui, eccles_gui_obj_t obj, int a0, int a1);
```

Arc: sweep from a0 to a1 degrees (default 135 .. 405, the usual gauge), ring thickness in pixels

### `eccles_gui_arc_set_thickness`

```c
eccles_gui_status_t eccles_gui_arc_set_thickness(eccles_gui_t *gui, eccles_gui_obj_t obj, int thickness);
```

### `eccles_gui_list_set_items`

```c
eccles_gui_status_t eccles_gui_list_set_items(eccles_gui_t *gui, eccles_gui_obj_t obj, const char *const *items, int count);
```

### `eccles_gui_list_set_selected`

```c
eccles_gui_status_t eccles_gui_list_set_selected(eccles_gui_t *gui, eccles_gui_obj_t obj, int index);
```

-1 clears

### `eccles_gui_list_scroll_to`

```c
eccles_gui_status_t eccles_gui_list_scroll_to(eccles_gui_t *gui, eccles_gui_obj_t obj, int pixels);
```

### `eccles_gui_custom_set_draw`

```c
eccles_gui_status_t eccles_gui_custom_set_draw(eccles_gui_t *gui, eccles_gui_obj_t obj, eccles_gui_draw_fn fn, void *user);
```

## Input

### `eccles_gui_touch`

```c
void eccles_gui_touch(eccles_gui_t *gui, int x, int y, bool pressed);
```

Feed the touch panel. call it every time you poll (pressed = finger is down, x,y = panel position). it does the hit test, sends the events and redraws pressed widgets. one finger only

## 3D scenes (view3d widget)

### `eccles_gui_view3d_get`

```c
eccles_gui_view3d_t *eccles_gui_view3d_get(eccles_gui_t *gui, eccles_gui_obj_t obj);
```

NULL if obj is not a view3d

### `eccles_gui_view3d_add`

```c
int eccles_gui_view3d_add(eccles_gui_t *gui, eccles_gui_obj_t obj, const eccles_gui_mesh_t *mesh, eccles_gui_color_t color);
```

Add a mesh to the scene. returns its object index or -1 when the scene is full

### `eccles_gui_view3d_set_transform`

```c
eccles_gui_status_t eccles_gui_view3d_set_transform(eccles_gui_t *gui, eccles_gui_obj_t obj, int index, int32_t x, int32_t y, int32_t z, uint16_t yaw, uint16_t pitch, uint16_t roll, int32_t scale);
```

### `eccles_gui_view3d_set_camera`

```c
eccles_gui_status_t eccles_gui_view3d_set_camera(eccles_gui_t *gui, eccles_gui_obj_t obj, int32_t x, int32_t y, int32_t z, uint16_t yaw, uint16_t pitch, int focal);
```

### `eccles_gui_view3d_set_light`

```c
eccles_gui_status_t eccles_gui_view3d_set_light(eccles_gui_t *gui, eccles_gui_obj_t obj, int dx, int dy, int dz, uint8_t ambient);
```

### `eccles_gui_view3d_set_mode`

```c
eccles_gui_status_t eccles_gui_view3d_set_mode(eccles_gui_t *gui, eccles_gui_obj_t obj, int index, uint8_t mode);
```

### `eccles_gui_view3d_set_texture`

```c
eccles_gui_status_t eccles_gui_view3d_set_texture(eccles_gui_t *gui, eccles_gui_obj_t obj, int index, const eccles_gui_image_t *tex);
```

### `eccles_gui_view3d_changed`

```c
void eccles_gui_view3d_changed(eccles_gui_t *gui, eccles_gui_obj_t obj);
```

Call after you changed the scene through the pointer from eccles_gui_view3d_get (the setters above already do it)

## Animation

### `eccles_gui_anim_start`

```c
eccles_gui_status_t eccles_gui_anim_start(eccles_gui_t *gui, const eccles_gui_anim_t *anim);
```

Start an animation. one with the same (obj, prop, index) is replaced. needs eccles_gui_update() to be running

### `eccles_gui_anim_cancel`

```c
int eccles_gui_anim_cancel(eccles_gui_t *gui, eccles_gui_obj_t obj, uint8_t prop);
```

Stop animations of a widget (prop 0 = all of its properties). the value stays where it is. returns how many were stopped

### `eccles_gui_anim_cancel_target`

```c
int eccles_gui_anim_cancel_target(eccles_gui_t *gui, void *target);
```

Stop the custom animations that use this target

### `eccles_gui_anim_count`

```c
int eccles_gui_anim_count(const eccles_gui_t *gui);
```

### `eccles_gui_ease`

```c
int32_t eccles_gui_ease(uint8_t ease, int32_t t);
```

The easing curve itself: t and the result are 0..1024 (the result can go past 1024 for OUT_BACK)
