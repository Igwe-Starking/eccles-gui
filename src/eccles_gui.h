/*
 * MIT License
 *
 * Copyright (c) 2025-2026 Nwobodo Ecclesiastes Chidera
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

//eccles_gui, a ui engine for microcontrollers (plain C99) that draws 2d widgets and real 3d
//one header, one .c file and one config header, no heap, no libm, no floats, no dependencies
//the same code runs on esp32, stm32, rp2040, nrf52, arduino boards or your pc
//
//the whole idea is BAND RENDERING. there is no framebuffer. the engine keeps a tree of widgets, remembers which
//rectangles changed (dirty rects), and redraws only those, a few lines at a time, into a small buffer you give it
//(for example 750 x 16 pixels = 24 KB for a 750 x 1334 phone panel instead of the 2 MB a full frame needs)
//every finished band is handed to your flush() callback, that is where your spi/qspi/rgb/dma driver goes
//
//what it draws: rounded rects, gradients, shadows, bevels, anti aliased lines, arcs and circles, bitmap text (utf-8,
//built in 5x8 font + your own 1/4/8 bpp fonts), images (rgb565, color key, a8, a4, a1 icons)
//widgets: panel, label, button, image, slider, switch, checkbox, progress, arc gauge, scrolling list, custom, view3d
//input: touch with click, long press, drag, swipe, kinetic list scroll. animations with easing. row/column layout
//3d: a software rasteriser with flat/smooth shading, textures, wireframe and an optional z buffer, drawn as a widget
//(view3d) or anywhere inside your own draw callback. all fixed point (Q12 world, Q15 sine, 16 bit angles)
//
//pixel format is rgb565 (ECCLES_GUI_SWAP_BYTES for panels that want the high byte first)
//all coordinates are int16, (0,0) is the top left of the panel, the x/y/w/h of every call are in pixels
//
//single threaded: call everything from ONE task (the ui task) or protect it with your own mutex
//all the config macros are described in eccles_gui_config.h (or pass them with -D), every file must see the same ones

#ifndef ECCLES_GUI_H
#define ECCLES_GUI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

//configuration file - same mechanism as eccles_wasm_config.h.
#if defined(__has_include)
  #if __has_include("eccles_gui_config.h")
    #include "eccles_gui_config.h"
  #endif
#endif

#define ECCLES_GUI_VERSION_MAJOR 1
#define ECCLES_GUI_VERSION_MINOR 0
#define ECCLES_GUI_VERSION_PATCH 0
#define ECCLES_GUI_VERSION_STRING "1.0.0"

#ifdef __cplusplus
extern "C" {
#endif

//platform detection (only used to pick the internal ram attribute for ECCLES_GUI_EXEC_IRAM)

#define ECCLES_GUI_PLAT_NONE      0
#define ECCLES_GUI_PLAT_HOST      1
#define ECCLES_GUI_PLAT_ARDUINO   2
#define ECCLES_GUI_PLAT_ESP_IDF   3
#define ECCLES_GUI_PLAT_PICO      4
#define ECCLES_GUI_PLAT_STM32_HAL 5
#define ECCLES_GUI_PLAT_ZEPHYR    6

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
  #define ECCLES_GUI_IS_ESP 1
#else
  #define ECCLES_GUI_IS_ESP 0
#endif

#ifndef ECCLES_GUI_PLATFORM
  #if defined(ARDUINO)
    #define ECCLES_GUI_PLATFORM ECCLES_GUI_PLAT_ARDUINO
  #elif defined(ESP_PLATFORM)
    #define ECCLES_GUI_PLATFORM ECCLES_GUI_PLAT_ESP_IDF
  #elif defined(PICO_BUILD) || defined(PICO_ON_DEVICE)
    #define ECCLES_GUI_PLATFORM ECCLES_GUI_PLAT_PICO
  #elif defined(__ZEPHYR__)
    #define ECCLES_GUI_PLATFORM ECCLES_GUI_PLAT_ZEPHYR
  #elif defined(USE_HAL_DRIVER)
    #define ECCLES_GUI_PLATFORM ECCLES_GUI_PLAT_STM32_HAL
  #elif defined(__unix__) || defined(__APPLE__) || defined(_WIN32)
    #define ECCLES_GUI_PLATFORM ECCLES_GUI_PLAT_HOST
  #else
    #define ECCLES_GUI_PLATFORM ECCLES_GUI_PLAT_NONE
  #endif
#endif

#if defined(__AVR__) || defined(__MSP430__) || defined(__STM8__) || defined(__C51__) || defined(__CX51__)
  #error "eccles_gui: 8/16-bit MCU - the engine needs a 32-bit core (it does int32 math and keeps a few KB of state)."
#endif

//tunables (see eccles_gui_config.h for what each one costs)

//features that can be compiled out (define them, the names are the same in the config file)
#ifdef ECCLES_GUI_NO_WIDGETS
  #define ECCLES_GUI_HAS_WIDGETS 0
#else
  #define ECCLES_GUI_HAS_WIDGETS 1
#endif
#ifdef ECCLES_GUI_NO_3D
  #define ECCLES_GUI_HAS_3D 0
#else
  #define ECCLES_GUI_HAS_3D 1
#endif
#ifdef ECCLES_GUI_NO_ANIM
  #define ECCLES_GUI_HAS_ANIM 0
#else
  #define ECCLES_GUI_HAS_ANIM 1
#endif
#ifdef ECCLES_GUI_NO_AA
  #define ECCLES_GUI_HAS_AA 0
#else
  #define ECCLES_GUI_HAS_AA 1
#endif
#ifdef ECCLES_GUI_NO_SHADOW
  #define ECCLES_GUI_HAS_SHADOW 0
#else
  #define ECCLES_GUI_HAS_SHADOW 1
#endif
#ifdef ECCLES_GUI_NO_BUILTIN_FONT
  #define ECCLES_GUI_HAS_BUILTIN_FONT 0
#else
  #define ECCLES_GUI_HAS_BUILTIN_FONT 1
#endif
#if defined(ECCLES_GUI_NO_BUILTIN_MESHES) || !ECCLES_GUI_HAS_3D
  #define ECCLES_GUI_HAS_BUILTIN_MESHES 0
#else
  #define ECCLES_GUI_HAS_BUILTIN_MESHES 1
#endif

#ifndef ECCLES_GUI_SWAP_BYTES
  #define ECCLES_GUI_SWAP_BYTES 0  //1: every flushed pixel has its two bytes swapped (spi panels that want the high byte first)
#endif
#ifndef ECCLES_GUI_MAX_WIDGETS
  #define ECCLES_GUI_MAX_WIDGETS 48
#endif
#ifndef ECCLES_GUI_MAX_STYLES
  #define ECCLES_GUI_MAX_STYLES 16
#endif
#ifndef ECCLES_GUI_MAX_FONTS
  #define ECCLES_GUI_MAX_FONTS 8
#endif
#ifndef ECCLES_GUI_MAX_DIRTY
  #define ECCLES_GUI_MAX_DIRTY 8
#endif
#ifndef ECCLES_GUI_MAX_ANIMS
  #define ECCLES_GUI_MAX_ANIMS 8
#endif
#ifndef ECCLES_GUI_MAX_VIEWS3D
  #define ECCLES_GUI_MAX_VIEWS3D 2
#endif
#ifndef ECCLES_GUI_3D_MAX_OBJECTS
  #define ECCLES_GUI_3D_MAX_OBJECTS 4  //meshes per view3d
#endif
#ifndef ECCLES_GUI_3D_MAX_VERTS
  #define ECCLES_GUI_3D_MAX_VERTS 192  //projected vertices cached per frame (all objects of one view)
#endif
#ifndef ECCLES_GUI_3D_MAX_FACES
  #define ECCLES_GUI_3D_MAX_FACES 256  //visible faces sorted per frame (all objects of one view)
#endif
#ifndef ECCLES_GUI_3D_ZBUFFER
  #define ECCLES_GUI_3D_ZBUFFER 1  //1: eccles_gui_desc_t.zbuf is used when you give one. 0 compiles the depth test out
#endif
#ifndef ECCLES_GUI_3D_TEXTURES
  #define ECCLES_GUI_3D_TEXTURES 1  //0 compiles out textured faces
#endif
#ifndef ECCLES_GUI_TOUCH_SLOP
  #define ECCLES_GUI_TOUCH_SLOP 10  //pixels a finger may wander before a press stops being a click
#endif
#ifndef ECCLES_GUI_SWIPE_MIN
  #define ECCLES_GUI_SWIPE_MIN 48  //pixels of travel for a swipe gesture
#endif
#ifndef ECCLES_GUI_LONG_PRESS_MS
  #define ECCLES_GUI_LONG_PRESS_MS 500
#endif
#ifndef ECCLES_GUI_EXEC_IRAM
  #define ECCLES_GUI_EXEC_IRAM 0  //1 puts the hot pixel loops in internal ram (see the config file)
#endif

#if (ECCLES_GUI_MAX_WIDGETS) < 1 || (ECCLES_GUI_MAX_WIDGETS) > 65000
  #error "eccles_gui: ECCLES_GUI_MAX_WIDGETS must be in 1..65000."
#endif
#if (ECCLES_GUI_MAX_STYLES) < 8 || (ECCLES_GUI_MAX_STYLES) > 255
  #error "eccles_gui: ECCLES_GUI_MAX_STYLES must be in 8..255 (the built in theme uses 8 slots)."
#endif
#if (ECCLES_GUI_MAX_FONTS) < 4 || (ECCLES_GUI_MAX_FONTS) > 255
  #error "eccles_gui: ECCLES_GUI_MAX_FONTS must be in 4..255 (the built in font uses 4 slots)."
#endif
#if (ECCLES_GUI_MAX_DIRTY) < 1 || (ECCLES_GUI_MAX_DIRTY) > 64
  #error "eccles_gui: ECCLES_GUI_MAX_DIRTY must be in 1..64."
#endif
#if (ECCLES_GUI_MAX_ANIMS) < 0 || (ECCLES_GUI_MAX_ANIMS) > 255
  #error "eccles_gui: ECCLES_GUI_MAX_ANIMS must be in 0..255."
#endif
#if (ECCLES_GUI_MAX_VIEWS3D) < 0 || (ECCLES_GUI_MAX_VIEWS3D) > 32
  #error "eccles_gui: ECCLES_GUI_MAX_VIEWS3D must be in 0..32."
#endif
#if (ECCLES_GUI_3D_MAX_OBJECTS) < 1 || (ECCLES_GUI_3D_MAX_OBJECTS) > 8
  #error "eccles_gui: ECCLES_GUI_3D_MAX_OBJECTS must be in 1..8."
#endif
#if (ECCLES_GUI_3D_MAX_VERTS) < 8 || (ECCLES_GUI_3D_MAX_VERTS) > 65535
  #error "eccles_gui: ECCLES_GUI_3D_MAX_VERTS must be in 8..65535."
#endif
#if (ECCLES_GUI_3D_MAX_FACES) < 4 || (ECCLES_GUI_3D_MAX_FACES) > 8191
  #error "eccles_gui: ECCLES_GUI_3D_MAX_FACES must be in 4..8191."
#endif
#if ECCLES_GUI_HAS_3D && (ECCLES_GUI_MAX_VIEWS3D) > 0 && !ECCLES_GUI_HAS_WIDGETS
  #error "eccles_gui: view3d widgets need widgets. Use ECCLES_GUI_MAX_VIEWS3D=0 (eccles_gui_draw_view3d still works) or drop ECCLES_GUI_NO_WIDGETS."
#endif

//status / error types

//what the functions that can fail return, same first 3 values as eccles_wasm_status_t
typedef enum {
    ECCLES_GUI_SUCCESSFUL = 0,  //did it
    ECCLES_GUI_FAILED = 1,  //tried and failed (pool full, bad handle, bad argument), see eccles_gui_last_error()
    ECCLES_GUI_UNSUPPORTED = 2  //that feature is compiled out (ECCLES_GUI_NO_3D, ...)
} eccles_gui_status_t;

//detailed reason behind the last FAILED/UNSUPPORTED - eccles_gui_last_error()
typedef enum {
    ECCLES_GUI_OK = 0,
    ECCLES_GUI_ERR_BAD_ARG,  //null pointer, size out of range, ...
    ECCLES_GUI_ERR_NO_MEMORY,  //a pool is full (widgets, styles, fonts, anims, views); the pool name is in last_error_str
    ECCLES_GUI_ERR_BAD_OBJECT,  //the handle is not a live widget
    ECCLES_GUI_ERR_BAD_TYPE,  //the widget is of another kind (set_text on a slider)
    ECCLES_GUI_ERR_BUFFER,  //the band buffer is smaller than one line of the panel
    ECCLES_GUI_ERR_NO_FLUSH,  //no flush callback
    ECCLES_GUI_ERR_DISABLED,  //feature compiled out
    ECCLES_GUI_ERR_LIMIT  //something exceeded a limit (3d vertex/face cache, texture size)
} eccles_gui_error_t;

//basic types

typedef uint16_t eccles_gui_color_t;  //rgb565
typedef uint16_t eccles_gui_obj_t;  //widget handle, an index into the pool
#define ECCLES_GUI_NO_OBJ 0xFFFFu

//rectangles are half open: x1,y1 inside, x2,y2 outside (so width = x2 - x1)
typedef struct { int16_t x1, y1, x2, y2; } eccles_gui_area_t;

//rgb888 -> rgb565
#define ECCLES_GUI_RGB(r, g, b) ((eccles_gui_color_t)((((unsigned)(r) & 0xF8u) << 8) | (((unsigned)(g) & 0xFCu) << 3) | (((unsigned)(b) & 0xF8u) >> 3)))
//0xRRGGBB -> rgb565
#define ECCLES_GUI_HEX(h) ECCLES_GUI_RGB(((h) >> 16) & 0xFFu, ((h) >> 8) & 0xFFu, (h) & 0xFFu)
#define ECCLES_GUI_OPA_TRANSP 0
#define ECCLES_GUI_OPA_COVER  255

//angles are 16 bit "binary angles": 65536 = a full turn. the 2d arc api uses plain degrees
#define ECCLES_GUI_DEG(d) ((uint16_t)((((uint32_t)(d) % 360u) * 65536u) / 360u))
//world units are Q12: 4096 = 1.0
#define ECCLES_GUI_ONE 4096
#define ECCLES_GUI_FIX(f) ((int32_t)((f) * 4096.0))

//display / flush

//called with a finished band of w*h pixels (rgb565, row after row, no padding) that belongs at x,y on the panel
//the buffer stays valid until flush_wait() returns (async flush) or until flush() returns (the normal case). never keep the pointer
typedef void (*eccles_gui_flush_fn)(void *user, int x, int y, int w, int h, const uint16_t *pixels);
//optional. only needed when flush() starts a DMA and returns before the transfer ends: block here until the last
//flush() has finished. the engine calls it before it reuses a buffer and when a render pass ends
typedef void (*eccles_gui_flush_wait_fn)(void *user);

#define ECCLES_GUI_DESC_ASYNC_FLUSH (1u << 0)  //flush() returns before the pixels are sent, flush_wait is required
#define ECCLES_GUI_DESC_SWAP_BYTES  (1u << 1)  //swap the bytes of every pixel before flush (same as ECCLES_GUI_SWAP_BYTES=1 but at run time)

typedef struct {
    int16_t width, height;  //panel size in pixels
    uint16_t *buf;  //band buffer. must be 2-byte aligned (4 for dma on most chips)
    size_t buf_pixels;  //its size in PIXELS (not bytes), at least width. more pixels = fewer, bigger flushes = faster
    uint16_t *buf2;  //optional second buffer of the same size: the engine draws into one while the other is being sent
    uint16_t *zbuf;  //optional depth band for view3d (same pixel count as buf), without it 3d uses the painter's algorithm
    eccles_gui_flush_fn flush;
    eccles_gui_flush_wait_fn flush_wait;
    void *user;  //passed back to flush / flush_wait
    uint8_t flags;  //ECCLES_GUI_DESC_*
} eccles_gui_desc_t;

//fonts

//a glyph. the bitmap rows are packed MSB first and every row starts on a byte boundary
//(1 bpp: (w+7)/8 bytes per row, 4 bpp: (w+1)/2, 8 bpp: w)
typedef struct {
    uint32_t off;  //byte offset of the first row in the font bitmap
    uint8_t w, h;  //bitmap size
    int8_t ox;  //x of the bitmap relative to the pen
    int8_t oy;  //y of the top of the bitmap relative to the TOP of the line
    uint8_t adv;  //pen advance
} eccles_gui_glyph_t;

//codepoints first .. first+count-1 map to glyphs index .. index+count-1
typedef struct {
    uint32_t first;
    uint16_t count;
    uint16_t index;
} eccles_gui_range_t;

typedef struct {
    const eccles_gui_glyph_t *glyphs;
    const uint8_t *bitmap;
    const eccles_gui_range_t *ranges;
    uint8_t nranges;
    uint8_t line_height;  //distance between two baselines
    uint8_t bpp;  //1, 4 or 8 (alpha)
} eccles_gui_font_t;

//the built in font is registered in slots 0 (x1), 1 (x2), 2 (x3) and 3 (x4). eccles_gui_font_register() gives you the next free id
#define ECCLES_GUI_FONT_1X 0
#define ECCLES_GUI_FONT_2X 1
#define ECCLES_GUI_FONT_3X 2
#define ECCLES_GUI_FONT_4X 3
#if ECCLES_GUI_HAS_BUILTIN_FONT
//the built in font itself: register it again at another scale for big text, eccles_gui_font_register(gui, &eccles_gui_font_5x8, 8)
extern const eccles_gui_font_t eccles_gui_font_5x8;
#endif

//images

typedef enum {
    ECCLES_GUI_IMG_RGB565 = 0,  //opaque pixels
    ECCLES_GUI_IMG_RGB565_KEY = 1,  //pixels equal to `key` are skipped
    ECCLES_GUI_IMG_A8 = 2,  //8 bit alpha mask painted in the color you pass (icons, masks)
    ECCLES_GUI_IMG_A4 = 3,  //4 bit alpha, high nibble first, rows start on a byte boundary
    ECCLES_GUI_IMG_A1 = 4  //1 bit mask, msb first, rows start on a byte boundary
} eccles_gui_imgfmt_t;

typedef struct {
    const void *data;
    uint16_t w, h;
    uint8_t fmt;  //eccles_gui_imgfmt_t
    uint16_t key;  //color key for ECCLES_GUI_IMG_RGB565_KEY
} eccles_gui_image_t;

//draw context

struct eccles_gui;
typedef struct eccles_gui eccles_gui_t;

//what every draw call paints into: one band of the panel. you get one in your custom draw callbacks
//all coordinates you pass to the eccles_gui_draw_* functions are PANEL coordinates, anything outside dc->clip is skipped
typedef struct eccles_gui_dc {
    eccles_gui_t *gui;
    uint16_t *buf;  //pixels of the band, row stride = stride
    uint16_t *zbuf;  //depth of the band or NULL
    int16_t stride;
    int16_t x0, y0;  //panel position of buf[0]
    eccles_gui_area_t clip;  //panel coordinates, always inside the band
} eccles_gui_dc_t;

//draws one region. called once per band, with the dc already set up. area is the region being redrawn
typedef void (*eccles_gui_draw_fn)(eccles_gui_dc_t *dc, const eccles_gui_area_t *area, void *user);

//styles

#define ECCLES_GUI_GRAD_NONE 0
#define ECCLES_GUI_GRAD_V    1  //bg at the top -> bg2 at the bottom
#define ECCLES_GUI_GRAD_H    2  //bg at the left -> bg2 at the right

typedef struct {
    eccles_gui_color_t bg;  //fill
    eccles_gui_color_t bg2;  //gradient end / slider and switch track / list separator
    eccles_gui_color_t border;
    eccles_gui_color_t text;
    eccles_gui_color_t accent;  //slider fill, switch on, checkmark, list selection, arc value
    uint8_t border_w;
    uint8_t radius;
    uint8_t pad;
    uint8_t shadow;  //soft shadow size in pixels, 0 = none (drawn under the widget, below it a little)
    uint8_t opa;  //255 = solid fill. 0 = no fill at all
    uint8_t font;  //font id
    uint8_t grad;  //ECCLES_GUI_GRAD_*
    int8_t bevel;  //>0 raised, <0 sunken, in pixels (classic 3d look)
} eccles_gui_style_t;

//named slots of the built in theme. you can change them with eccles_gui_style_set() or add your own after ECCLES_GUI_STYLE_USER
enum {
    ECCLES_GUI_STYLE_SCREEN = 0,  //the root: opaque background
    ECCLES_GUI_STYLE_PANEL,  //flat container, no fill
    ECCLES_GUI_STYLE_CARD,  //rounded raised surface with a shadow
    ECCLES_GUI_STYLE_BUTTON,
    ECCLES_GUI_STYLE_BUTTON_PRIMARY,  //accent colored
    ECCLES_GUI_STYLE_LABEL,  //text on nothing
    ECCLES_GUI_STYLE_LABEL_TITLE,  //2x font
    ECCLES_GUI_STYLE_LABEL_MUTED,
    ECCLES_GUI_STYLE_USER  //first slot free for your own styles
};

//a palette for the built in theme: eccles_gui_theme_apply() derives all 8 slots from it
typedef struct {
    eccles_gui_color_t bg, surface, surface2, border, text, muted, accent, on_accent;
} eccles_gui_palette_t;
extern const eccles_gui_palette_t eccles_gui_palette_dark;
extern const eccles_gui_palette_t eccles_gui_palette_light;

//widgets, events, animation

typedef enum {
    ECCLES_GUI_W_FREE = 0,
    ECCLES_GUI_W_PANEL,
    ECCLES_GUI_W_LABEL,
    ECCLES_GUI_W_BUTTON,
    ECCLES_GUI_W_IMAGE,
    ECCLES_GUI_W_SLIDER,
    ECCLES_GUI_W_SWITCH,
    ECCLES_GUI_W_CHECKBOX,
    ECCLES_GUI_W_PROGRESS,
    ECCLES_GUI_W_ARC,
    ECCLES_GUI_W_LIST,
    ECCLES_GUI_W_VIEW3D,
    ECCLES_GUI_W_CUSTOM
} eccles_gui_wtype_t;

typedef enum {
    ECCLES_GUI_EV_PRESSED = 1,  //finger went down on the widget
    ECCLES_GUI_EV_RELEASED,  //finger came up (always follows PRESSED)
    ECCLES_GUI_EV_CLICKED,  //pressed and released without wandering. value = list item index for lists
    ECCLES_GUI_EV_LONG_PRESSED,  //held for ECCLES_GUI_LONG_PRESS_MS, the click that would follow is cancelled
    ECCLES_GUI_EV_DRAG,  //finger moved: dx,dy since the last event, x,y = position
    ECCLES_GUI_EV_VALUE_CHANGED,  //slider moved, switch/checkbox toggled, list selection changed. value = new value
    ECCLES_GUI_EV_GESTURE  //swipe. value = ECCLES_GUI_DIR_*, goes to the widget (or its nearest ancestor with a callback)
} eccles_gui_ev_t;

#define ECCLES_GUI_DIR_LEFT  1
#define ECCLES_GUI_DIR_RIGHT 2
#define ECCLES_GUI_DIR_UP    3
#define ECCLES_GUI_DIR_DOWN  4

typedef struct {
    eccles_gui_ev_t code;
    eccles_gui_obj_t obj;  //the widget the event is about
    int16_t x, y;  //touch position (panel coordinates)
    int16_t dx, dy;
    int32_t value;
    void *user;  //the widget's user pointer
} eccles_gui_event_t;

typedef void (*eccles_gui_event_fn)(eccles_gui_t *gui, const eccles_gui_event_t *ev);

//alignment of a widget inside its parent (eccles_gui_obj_set_align). the x/y of the widget are then an offset from there
typedef enum {
    ECCLES_GUI_ALIGN_NONE = 0,  //x,y are the position inside the parent
    ECCLES_GUI_ALIGN_TOP_LEFT,
    ECCLES_GUI_ALIGN_TOP_MID,
    ECCLES_GUI_ALIGN_TOP_RIGHT,
    ECCLES_GUI_ALIGN_MID_LEFT,
    ECCLES_GUI_ALIGN_CENTER,
    ECCLES_GUI_ALIGN_MID_RIGHT,
    ECCLES_GUI_ALIGN_BOTTOM_LEFT,
    ECCLES_GUI_ALIGN_BOTTOM_MID,
    ECCLES_GUI_ALIGN_BOTTOM_RIGHT
} eccles_gui_align_t;

//label text modes
#define ECCLES_GUI_TEXT_LEFT    0
#define ECCLES_GUI_TEXT_CENTER  1
#define ECCLES_GUI_TEXT_RIGHT   2
#define ECCLES_GUI_TEXT_CLIP    0  //one line cut at the edge
#define ECCLES_GUI_TEXT_ELLIPSIS 4  //one line, ends in ... when it does not fit
#define ECCLES_GUI_TEXT_WRAP    8  //breaks lines at spaces (and at '\n')

//widget flags (read only for you, use the setters)
#define ECCLES_GUI_F_HIDDEN    (1u << 0)
#define ECCLES_GUI_F_DISABLED  (1u << 1)
#define ECCLES_GUI_F_CLICKABLE (1u << 2)
#define ECCLES_GUI_F_PRESSED   (1u << 3)
#define ECCLES_GUI_F_CHECKED   (1u << 4)

//layout directions for eccles_gui_obj_layout()
#define ECCLES_GUI_LAYOUT_ROW    0
#define ECCLES_GUI_LAYOUT_COLUMN 1

#if ECCLES_GUI_HAS_WIDGETS
//one widget = about 40 bytes on a 32 bit chip. you never touch the fields, they are here so the context can be a plain struct
typedef struct {
    int16_t x, y, w, h;  //position inside the parent (or an offset from the alignment point) and size
    eccles_gui_obj_t parent, child, next;  //tree links: first child, next sibling
    uint8_t type, flags, style, opa;
    uint8_t align;
    uint8_t aux;  //label text mode, arc thickness, switch/checkbox state is in flags
    union {
        struct { const char *text; } label;  //label, button, checkbox
        struct { const eccles_gui_image_t *img; } image;
        struct { int16_t val, min, max; } range;  //slider, progress
        struct { int16_t val, min, max, a0, a1; } arc;  //degrees
        struct { const char *const *items; uint16_t count, sel; int16_t scroll; } list;
        struct { uint8_t view; } v3d;
        struct { eccles_gui_draw_fn fn; } custom;
    } u;
    eccles_gui_event_fn cb;
    void *user;
} eccles_gui_widget_t;
#endif

//what an animation changes. the numbers are in the unit of the property (pixels, 0..255 opa, widget value, binary angle)
typedef enum {
    ECCLES_GUI_ANIM_X = 1,
    ECCLES_GUI_ANIM_Y,
    ECCLES_GUI_ANIM_W,
    ECCLES_GUI_ANIM_H,
    ECCLES_GUI_ANIM_OPA,
    ECCLES_GUI_ANIM_VALUE,  //slider / progress / arc
    ECCLES_GUI_ANIM_V3D_YAW,  //view3d object `index`, binary angle
    ECCLES_GUI_ANIM_V3D_PITCH,
    ECCLES_GUI_ANIM_V3D_ROLL,
    ECCLES_GUI_ANIM_V3D_SCALE,  //Q12
    ECCLES_GUI_ANIM_CUSTOM  //calls exec(gui, target, value) every frame
} eccles_gui_anim_prop_t;

typedef enum {
    ECCLES_GUI_EASE_LINEAR = 0,
    ECCLES_GUI_EASE_IN,  //quad
    ECCLES_GUI_EASE_OUT,
    ECCLES_GUI_EASE_IN_OUT,
    ECCLES_GUI_EASE_OUT_CUBIC,
    ECCLES_GUI_EASE_OUT_BACK,  //overshoots a little and settles
    ECCLES_GUI_EASE_OUT_BOUNCE
} eccles_gui_ease_t;

typedef void (*eccles_gui_anim_exec_fn)(eccles_gui_t *gui, void *target, int32_t value);
typedef void (*eccles_gui_anim_done_fn)(eccles_gui_t *gui, void *target);

#define ECCLES_GUI_ANIM_REPEAT_FOREVER 0xFFFFu

typedef struct {
    eccles_gui_obj_t obj;  //the widget (ignored for CUSTOM)
    uint8_t prop;  //eccles_gui_anim_prop_t
    uint8_t index;  //view3d object index for the V3D_* properties
    uint8_t ease;  //eccles_gui_ease_t
    bool yoyo;  //play back and forth instead of restarting
    int32_t from, to;
    uint16_t duration_ms;
    uint16_t delay_ms;
    uint16_t repeat;  //extra plays after the first one, ECCLES_GUI_ANIM_REPEAT_FOREVER = never ends
    eccles_gui_anim_exec_fn exec;  //CUSTOM only
    eccles_gui_anim_done_fn done;  //optional, called when it finishes (never for endless ones)
    void *target;  //passed to exec / done
} eccles_gui_anim_t;

typedef struct {
    eccles_gui_anim_t a;
    uint32_t elapsed;
    uint8_t used;
} eccles_gui_anim_slot_t;

//3d: meshes, objects, views

//a mesh lives in flash and is never copied. vertices are int16 in Q12 (4096 = 1.0), keep them within about +-2.0
//faces are counter clockwise seen from the outside (that is what gets drawn, the back is culled)
typedef struct {
    const int16_t *verts;  //x,y,z per vertex
    const uint16_t *faces;  //3 vertex indices per face
    const int8_t *normals;  //optional: 3 per face, unit length * 127. NULL = computed at run time (slower)
    const int8_t *vnormals;  //optional: 3 per vertex, same scale, needed for smooth shading
    const uint16_t *colors;  //optional: one rgb565 per face (overrides the object color)
    const uint8_t *uv;  //optional: 2 per vertex (u,v in 0..255), needed for textures
    uint16_t nverts, nfaces;
} eccles_gui_mesh_t;

#define ECCLES_GUI_3D_FLAT     0  //one light value per face
#define ECCLES_GUI_3D_SMOOTH   1  //light value interpolated across the face (needs vnormals, else flat)
#define ECCLES_GUI_3D_UNLIT    2  //plain color, no light
#define ECCLES_GUI_3D_WIRE     3  //edges only
#define ECCLES_GUI_3D_TEXTURED 4  //texture (needs uv + a texture) modulated by the light, affine mapping

#define ECCLES_GUI_3D_F_VISIBLE      (1u << 0)
#define ECCLES_GUI_3D_F_DOUBLE_SIDED (1u << 1)  //draw back faces too (flat cards, leaves)

typedef struct {
    const eccles_gui_mesh_t *mesh;  //NULL = slot unused
    const eccles_gui_image_t *tex;  //rgb565, width and height powers of two, up to 256
    int32_t x, y, z;  //position, Q12
    uint16_t yaw, pitch, roll;  //binary angles. rotation order: roll (z), pitch (x), yaw (y)
    int32_t scale;  //Q12, 4096 = 1.0. keep (largest vertex * scale) below 8.0
    eccles_gui_color_t color;
    uint8_t mode;  //ECCLES_GUI_3D_*
    uint8_t flags;  //ECCLES_GUI_3D_F_*
} eccles_gui_obj3d_t;

typedef struct {
    eccles_gui_obj3d_t obj[ECCLES_GUI_3D_MAX_OBJECTS];
    int32_t cam_x, cam_y, cam_z;  //camera position, Q12. the camera looks along +z when yaw = pitch = 0
    uint16_t cam_yaw, cam_pitch;
    int16_t focal;  //focal length in pixels (bigger = narrower view). about the view height is a normal lens
    int32_t near_z;  //Q12, faces with a vertex closer than this are dropped
    int8_t light[3];  //direction TO the light in world space (any length, it is normalised on use)
    uint8_t ambient;  //0..255 minimum light
    eccles_gui_color_t bg;
    uint8_t flags;  //ECCLES_GUI_3D_VIEW_*
} eccles_gui_view3d_t;

#define ECCLES_GUI_3D_VIEW_TRANSPARENT (1u << 0)  //do not clear to bg first
#define ECCLES_GUI_3D_VIEW_NO_CULL     (1u << 1)  //keep back faces everywhere

//built in meshes (flash only, ECCLES_GUI_NO_BUILTIN_MESHES removes them). all fit in a unit cube, have normals, uv and vnormals
#if ECCLES_GUI_HAS_BUILTIN_MESHES
extern const eccles_gui_mesh_t eccles_gui_mesh_cube;  //24 verts, 12 faces, uv per face
extern const eccles_gui_mesh_t eccles_gui_mesh_pyramid;  //square pyramid
extern const eccles_gui_mesh_t eccles_gui_mesh_cylinder;  //16 sides, smooth normals
extern const eccles_gui_mesh_t eccles_gui_mesh_sphere;  //subdivided icosahedron, 42 verts, 80 faces
extern const eccles_gui_mesh_t eccles_gui_mesh_torus;  //16 x 10
extern const eccles_gui_mesh_t eccles_gui_mesh_card;  //a flat 4x4 grid quad with uv, for textured cards (use DOUBLE_SIDED)
#endif

//stats

typedef struct {
    uint32_t frames;  //eccles_gui_render() passes that drew something
    uint32_t pixels_rendered;
    uint32_t pixels_flushed;
    uint32_t flushes;
    uint16_t widgets_used;
    uint16_t widgets_peak;
    uint8_t dirty_peak;
    uint8_t anims_active;
    uint16_t faces_drawn;  //last frame
    uint16_t faces_dropped;  //last frame: culled by near plane / off screen / cache full
    uint32_t ram_bytes;  //sizeof(eccles_gui_t)
} eccles_gui_stats_t;

//the context. put it in a static, you never touch the fields (they are here so no malloc is needed)

#if ECCLES_GUI_HAS_3D
typedef struct {
    int16_t x, y;  //screen position
    uint16_t iz;  //1/z for the depth test
    uint8_t shade;  //0..255 light at this vertex
    uint8_t ok;  //0 = behind the near plane
} eccles_gui_pv_t;

typedef struct {
    int32_t key;  //sum of the 1/z of the 3 vertices, smaller = farther away
    uint16_t ref;  //object index (3 bits) << 13 | face index
    uint8_t shade;
    uint8_t pad;
} eccles_gui_pf_t;
#endif

struct eccles_gui {
    eccles_gui_desc_t d;
    uint8_t ready, error, cur, pending;
    uint8_t ndirty;
    eccles_gui_area_t dirty[ECCLES_GUI_MAX_DIRTY];
    uint32_t frame;
    eccles_gui_stats_t stats;
    eccles_gui_error_t last_error;
    const char *error_str;
    struct { const eccles_gui_font_t *f; uint8_t scale; } fonts[ECCLES_GUI_MAX_FONTS];
    uint8_t nfonts;
#if ECCLES_GUI_HAS_WIDGETS
    eccles_gui_style_t styles[ECCLES_GUI_MAX_STYLES];
    uint8_t nstyles;
    eccles_gui_widget_t w[ECCLES_GUI_MAX_WIDGETS];
    uint16_t nwidgets;
    //touch
    uint8_t t_down, t_moved, t_long, t_swipe_done;
    eccles_gui_obj_t t_obj;
    int16_t t_x0, t_y0, t_x, t_y;
    uint32_t t_ms;
    int16_t t_vy;  //list scroll velocity, pixels per 16 ms
    int16_t t_vacc;
    uint32_t t_vt;
    eccles_gui_obj_t fling_obj;
    int16_t fling_v;
    uint32_t fling_acc;
#endif
    uint32_t now_ms;
    uint8_t have_time;
#if ECCLES_GUI_HAS_ANIM && ECCLES_GUI_MAX_ANIMS > 0
    eccles_gui_anim_slot_t anims[ECCLES_GUI_MAX_ANIMS];
#endif
#if ECCLES_GUI_HAS_3D
  #if ECCLES_GUI_MAX_VIEWS3D > 0
    eccles_gui_view3d_t views[ECCLES_GUI_MAX_VIEWS3D];
    uint8_t view_used[ECCLES_GUI_MAX_VIEWS3D];
  #endif
    struct {
        const eccles_gui_view3d_t *scene;
        uint32_t frame;
        int16_t vx, vy, vw, vh;
        uint16_t nv, nf;
        uint16_t vbase[ECCLES_GUI_3D_MAX_OBJECTS];
        eccles_gui_pv_t pv[ECCLES_GUI_3D_MAX_VERTS];
        eccles_gui_pf_t pf[ECCLES_GUI_3D_MAX_FACES];
        uint16_t dropped;
    } r3;
#endif
};

//init / loop

//set up the context. every pool is empty afterwards (and, with widgets, there is a root widget the size of the panel)
//the built in theme is the dark palette. fails with ECCLES_GUI_ERR_BUFFER if buf_pixels < width, ERR_NO_FLUSH without a flush callback
eccles_gui_status_t eccles_gui_init(eccles_gui_t *gui, const eccles_gui_desc_t *desc);

//forget everything. (there is nothing to free, this just marks the context unusable)
void eccles_gui_deinit(eccles_gui_t *gui);

//the normal main loop call. now_ms is any monotonic millisecond counter (it may wrap), it drives animations, long press
//and kinetic scrolling. then it redraws what changed. returns the number of pixels that were flushed
uint32_t eccles_gui_update(eccles_gui_t *gui, uint32_t now_ms);

//redraw every dirty rectangle now (no timekeeping). returns the number of pixels flushed
uint32_t eccles_gui_render(eccles_gui_t *gui);

//immediate mode: redraw one region of the panel with your own function, band by band, and flush it
//your function must paint every pixel of the area (start with eccles_gui_draw_fill). no widgets involved
uint32_t eccles_gui_render_area(eccles_gui_t *gui, int x, int y, int w, int h, eccles_gui_draw_fn fn, void *user);

//mark a rectangle as changed, the next render redraws it. clipped to the panel, overlapping ones are merged
void eccles_gui_invalidate(eccles_gui_t *gui, int x, int y, int w, int h);
void eccles_gui_invalidate_all(eccles_gui_t *gui);
bool eccles_gui_is_dirty(const eccles_gui_t *gui);

eccles_gui_error_t eccles_gui_last_error(const eccles_gui_t *gui);
const char *eccles_gui_last_error_str(const eccles_gui_t *gui);  //which pool ran out (or "")
const char *eccles_gui_error_name(eccles_gui_error_t e);
eccles_gui_stats_t eccles_gui_get_stats(const eccles_gui_t *gui);

//fonts and text

//add a font (it stays in flash, only the pointer is kept). scale 1..8 enlarges it by that integer factor. returns the id or -1
int eccles_gui_font_register(eccles_gui_t *gui, const eccles_gui_font_t *font, uint8_t scale);
//width in pixels of the first line of text (up to '\n')
int eccles_gui_text_width(const eccles_gui_t *gui, uint8_t font, const char *text);
//height of the text when it is wrapped at max_w pixels (0 = no wrapping, only '\n' breaks lines)
int eccles_gui_text_height(const eccles_gui_t *gui, uint8_t font, const char *text, int max_w);
//line height of a font in pixels
int eccles_gui_font_height(const eccles_gui_t *gui, uint8_t font);

//drawing (use inside a draw callback, or let the widgets do it)
//opa is 0..255. everything is clipped to dc->clip

void eccles_gui_draw_fill(eccles_gui_dc_t *dc, int x, int y, int w, int h, eccles_gui_color_t c, uint8_t opa);
//rounded rectangle, anti aliased. a radius of w/2 makes a pill or a disc
void eccles_gui_draw_rrect(eccles_gui_dc_t *dc, int x, int y, int w, int h, int r, eccles_gui_color_t c, uint8_t opa);
//outline of a rounded rectangle, bw pixels thick, drawn inside x,y,w,h
void eccles_gui_draw_border(eccles_gui_dc_t *dc, int x, int y, int w, int h, int r, int bw, eccles_gui_color_t c, uint8_t opa);
//rounded rectangle with a gradient (ECCLES_GUI_GRAD_V / _H)
void eccles_gui_draw_gradient(eccles_gui_dc_t *dc, int x, int y, int w, int h, int r, eccles_gui_color_t c1, eccles_gui_color_t c2, uint8_t dir, uint8_t opa);
//soft shadow of a rounded rectangle (size = blur in pixels), drawn around/below it, offset down by size/3
void eccles_gui_draw_shadow(eccles_gui_dc_t *dc, int x, int y, int w, int h, int r, int size, uint8_t opa);
void eccles_gui_draw_disc(eccles_gui_dc_t *dc, int cx, int cy, int r, eccles_gui_color_t c, uint8_t opa);
//anti aliased line, width in pixels, endpoints are pixel centers
void eccles_gui_draw_line(eccles_gui_dc_t *dc, int x0, int y0, int x1, int y1, int width, eccles_gui_color_t c, uint8_t opa);
//ring segment from a0 to a1 degrees (0 = 3 o'clock, growing clockwise), thickness pixels inside radius r. rounded caps if round_caps
void eccles_gui_draw_arc(eccles_gui_dc_t *dc, int cx, int cy, int r, int thickness, int a0, int a1, bool round_caps, eccles_gui_color_t c, uint8_t opa);
//filled triangle (no anti aliasing)
void eccles_gui_draw_triangle(eccles_gui_dc_t *dc, int x0, int y0, int x1, int y1, int x2, int y2, eccles_gui_color_t c, uint8_t opa);
//one pixel
void eccles_gui_draw_pixel(eccles_gui_dc_t *dc, int x, int y, eccles_gui_color_t c, uint8_t opa);
//text: utf-8, '\n' starts a new line. (x,y) is the top left. returns the width of the widest line
int eccles_gui_draw_text(eccles_gui_dc_t *dc, int x, int y, uint8_t font, const char *text, eccles_gui_color_t c, uint8_t opa);
//text in a box with alignment (ECCLES_GUI_TEXT_*), wrapping / ellipsis, top aligned
void eccles_gui_draw_text_box(eccles_gui_dc_t *dc, const eccles_gui_area_t *box, uint8_t font, const char *text, eccles_gui_color_t c, uint8_t opa, uint8_t mode);
//an image. for the A* formats the image is painted in color c
void eccles_gui_draw_image(eccles_gui_dc_t *dc, int x, int y, const eccles_gui_image_t *img, eccles_gui_color_t c, uint8_t opa);

//colors / math helpers
//t = 0 gives a, 255 gives b
eccles_gui_color_t eccles_gui_color_mix(eccles_gui_color_t a, eccles_gui_color_t b, uint8_t t);
//sine and cosine of a binary angle (65536 = 360 degrees) as Q15 (32767 = 1.0)
int16_t eccles_gui_sin(uint16_t angle);
int16_t eccles_gui_cos(uint16_t angle);
static inline eccles_gui_area_t eccles_gui_area(int x, int y, int w, int h) {
    eccles_gui_area_t a;
    a.x1 = (int16_t)x; a.y1 = (int16_t)y; a.x2 = (int16_t)(x + w); a.y2 = (int16_t)(y + h);
    return a;
}

#if ECCLES_GUI_HAS_3D

//3d drawing

//draws a scene (the objects of a view3d struct) into the viewport x,y,w,h of this band. works in any draw callback
//the geometry is projected once per frame and reused by every band, so call it with the same scene and viewport in every band
void eccles_gui_draw_view3d(eccles_gui_dc_t *dc, const eccles_gui_view3d_t *scene, int x, int y, int w, int h);
//fill a scene with defaults: camera 4 units back, focal = 1 view height, light from the top left front, no objects
void eccles_gui_view3d_defaults(eccles_gui_view3d_t *scene, int view_h);
#endif

#if ECCLES_GUI_HAS_WIDGETS

//styles

//copy a style into a slot (any slot < ECCLES_GUI_MAX_STYLES). the whole panel is redrawn
eccles_gui_status_t eccles_gui_style_set(eccles_gui_t *gui, uint8_t slot, const eccles_gui_style_t *style);
const eccles_gui_style_t *eccles_gui_style_get(const eccles_gui_t *gui, uint8_t slot);
//the first free slot at or after ECCLES_GUI_STYLE_USER, filled with `style`. returns the slot or -1
int eccles_gui_style_add(eccles_gui_t *gui, const eccles_gui_style_t *style);
//rebuild the 8 built in slots from a palette and redraw everything
void eccles_gui_theme_apply(eccles_gui_t *gui, const eccles_gui_palette_t *palette);

//widgets: creating

eccles_gui_obj_t eccles_gui_root(const eccles_gui_t *gui);  //the full panel widget that holds everything
//every create call returns ECCLES_GUI_NO_OBJ when the pool is full or the parent is bad (see eccles_gui_last_error)
//the new widget is on top of its siblings, at 0,0, 100x40 (labels fit their text), with the style that suits its kind
eccles_gui_obj_t eccles_gui_panel_create(eccles_gui_t *gui, eccles_gui_obj_t parent);
eccles_gui_obj_t eccles_gui_label_create(eccles_gui_t *gui, eccles_gui_obj_t parent, const char *text);
eccles_gui_obj_t eccles_gui_button_create(eccles_gui_t *gui, eccles_gui_obj_t parent, const char *text);
eccles_gui_obj_t eccles_gui_image_create(eccles_gui_t *gui, eccles_gui_obj_t parent, const eccles_gui_image_t *img);
eccles_gui_obj_t eccles_gui_slider_create(eccles_gui_t *gui, eccles_gui_obj_t parent, int min, int max, int value);
eccles_gui_obj_t eccles_gui_switch_create(eccles_gui_t *gui, eccles_gui_obj_t parent, bool on);
eccles_gui_obj_t eccles_gui_checkbox_create(eccles_gui_t *gui, eccles_gui_obj_t parent, const char *text, bool checked);
eccles_gui_obj_t eccles_gui_progress_create(eccles_gui_t *gui, eccles_gui_obj_t parent, int min, int max, int value);
eccles_gui_obj_t eccles_gui_arc_create(eccles_gui_t *gui, eccles_gui_obj_t parent, int min, int max, int value);
//a scrolling list of text rows, only the visible rows are drawn so it can be as long as you like. items are not copied
eccles_gui_obj_t eccles_gui_list_create(eccles_gui_t *gui, eccles_gui_obj_t parent, const char *const *items, int count);
eccles_gui_obj_t eccles_gui_custom_create(eccles_gui_t *gui, eccles_gui_obj_t parent, eccles_gui_draw_fn fn, void *user);
//a 3d scene. needs ECCLES_GUI_MAX_VIEWS3D > 0
eccles_gui_obj_t eccles_gui_view3d_create(eccles_gui_t *gui, eccles_gui_obj_t parent);

//widgets: the tree

//delete a widget and everything inside it. the root cannot be deleted
eccles_gui_status_t eccles_gui_obj_delete(eccles_gui_t *gui, eccles_gui_obj_t obj);
eccles_gui_status_t eccles_gui_obj_set_parent(eccles_gui_t *gui, eccles_gui_obj_t obj, eccles_gui_obj_t parent);
void eccles_gui_obj_to_front(eccles_gui_t *gui, eccles_gui_obj_t obj);
void eccles_gui_obj_to_back(eccles_gui_t *gui, eccles_gui_obj_t obj);
bool eccles_gui_obj_valid(const eccles_gui_t *gui, eccles_gui_obj_t obj);
eccles_gui_obj_t eccles_gui_obj_parent(const eccles_gui_t *gui, eccles_gui_obj_t obj);
eccles_gui_obj_t eccles_gui_obj_child(const eccles_gui_t *gui, eccles_gui_obj_t obj);  //first child (bottom of the stack)
eccles_gui_obj_t eccles_gui_obj_next(const eccles_gui_t *gui, eccles_gui_obj_t obj);  //next sibling
//topmost visible widget at a panel position (ECCLES_GUI_NO_OBJ if none). with only_clickable, skips the ones that cannot be touched
eccles_gui_obj_t eccles_gui_obj_at(const eccles_gui_t *gui, int x, int y, bool only_clickable);

//widgets: geometry and look. every setter redraws what it changed

void eccles_gui_obj_set_pos(eccles_gui_t *gui, eccles_gui_obj_t obj, int x, int y);
void eccles_gui_obj_set_size(eccles_gui_t *gui, eccles_gui_obj_t obj, int w, int h);
void eccles_gui_obj_set_area(eccles_gui_t *gui, eccles_gui_obj_t obj, int x, int y, int w, int h);
void eccles_gui_obj_set_align(eccles_gui_t *gui, eccles_gui_obj_t obj, eccles_gui_align_t align, int ox, int oy);
void eccles_gui_obj_set_style(eccles_gui_t *gui, eccles_gui_obj_t obj, uint8_t style);
void eccles_gui_obj_set_opa(eccles_gui_t *gui, eccles_gui_obj_t obj, uint8_t opa);  //0..255, multiplies with the style opa and the parent's
void eccles_gui_obj_set_hidden(eccles_gui_t *gui, eccles_gui_obj_t obj, bool hidden);
void eccles_gui_obj_set_enabled(eccles_gui_t *gui, eccles_gui_obj_t obj, bool enabled);
void eccles_gui_obj_set_clickable(eccles_gui_t *gui, eccles_gui_obj_t obj, bool clickable);
//absolute position and size in panel coordinates
eccles_gui_area_t eccles_gui_obj_get_area(const eccles_gui_t *gui, eccles_gui_obj_t obj);
//lay the children out in a row or column inside their parent: padding at the edges, gap between them.
//a child with size 0 on the main axis shares the remaining space equally. on the cross axis every child is centered
eccles_gui_status_t eccles_gui_obj_layout(eccles_gui_t *gui, eccles_gui_obj_t obj, int direction, int pad, int gap);
void eccles_gui_obj_set_user(eccles_gui_t *gui, eccles_gui_obj_t obj, void *user);
void *eccles_gui_obj_get_user(const eccles_gui_t *gui, eccles_gui_obj_t obj);
//one callback per widget, it gets every event of that widget
void eccles_gui_obj_on(eccles_gui_t *gui, eccles_gui_obj_t obj, eccles_gui_event_fn cb, void *user);
void eccles_gui_obj_invalidate(eccles_gui_t *gui, eccles_gui_obj_t obj);  //redraw this widget (call after changing text you edited in place)

//widgets: content

//label, button and checkbox. the text is NOT copied, keep it alive (a static string or your own buffer: after you
//change a buffer in place, call set_text again with the same pointer to redraw)
eccles_gui_status_t eccles_gui_set_text(eccles_gui_t *gui, eccles_gui_obj_t obj, const char *text);
//ECCLES_GUI_TEXT_LEFT/CENTER/RIGHT | ECCLES_GUI_TEXT_CLIP/ELLIPSIS/WRAP
eccles_gui_status_t eccles_gui_set_text_mode(eccles_gui_t *gui, eccles_gui_obj_t obj, uint8_t mode);
eccles_gui_status_t eccles_gui_image_set(eccles_gui_t *gui, eccles_gui_obj_t obj, const eccles_gui_image_t *img);
//slider / progress / arc range and value. a change from code does not send VALUE_CHANGED (a change by touch does)
eccles_gui_status_t eccles_gui_set_range(eccles_gui_t *gui, eccles_gui_obj_t obj, int min, int max);
eccles_gui_status_t eccles_gui_set_value(eccles_gui_t *gui, eccles_gui_obj_t obj, int value);
//slider/progress/arc value, switch/checkbox 0 or 1, list selected row (-1 = none)
int eccles_gui_get_value(const eccles_gui_t *gui, eccles_gui_obj_t obj);
eccles_gui_status_t eccles_gui_set_checked(eccles_gui_t *gui, eccles_gui_obj_t obj, bool checked);
bool eccles_gui_is_checked(const eccles_gui_t *gui, eccles_gui_obj_t obj);
//arc: sweep from a0 to a1 degrees (default 135 .. 405, the usual gauge), ring thickness in pixels
eccles_gui_status_t eccles_gui_arc_set_angles(eccles_gui_t *gui, eccles_gui_obj_t obj, int a0, int a1);
eccles_gui_status_t eccles_gui_arc_set_thickness(eccles_gui_t *gui, eccles_gui_obj_t obj, int thickness);
eccles_gui_status_t eccles_gui_list_set_items(eccles_gui_t *gui, eccles_gui_obj_t obj, const char *const *items, int count);
eccles_gui_status_t eccles_gui_list_set_selected(eccles_gui_t *gui, eccles_gui_obj_t obj, int index);  //-1 clears
eccles_gui_status_t eccles_gui_list_scroll_to(eccles_gui_t *gui, eccles_gui_obj_t obj, int pixels);
eccles_gui_status_t eccles_gui_custom_set_draw(eccles_gui_t *gui, eccles_gui_obj_t obj, eccles_gui_draw_fn fn, void *user);

//input

//feed the touch panel. call it every time you poll (pressed = finger is down, x,y = panel position). it does the hit test,
//sends the events and redraws pressed widgets. one finger only
void eccles_gui_touch(eccles_gui_t *gui, int x, int y, bool pressed);

#endif  //ECCLES_GUI_HAS_WIDGETS

#if ECCLES_GUI_HAS_3D && ECCLES_GUI_HAS_WIDGETS
//view3d: the scene lives inside the context, edit it with these or take the pointer
eccles_gui_view3d_t *eccles_gui_view3d_get(eccles_gui_t *gui, eccles_gui_obj_t obj);  //NULL if obj is not a view3d
//add a mesh to the scene. returns its object index or -1 when the scene is full
int eccles_gui_view3d_add(eccles_gui_t *gui, eccles_gui_obj_t obj, const eccles_gui_mesh_t *mesh, eccles_gui_color_t color);
eccles_gui_status_t eccles_gui_view3d_set_transform(eccles_gui_t *gui, eccles_gui_obj_t obj, int index, int32_t x, int32_t y, int32_t z,
                                                    uint16_t yaw, uint16_t pitch, uint16_t roll, int32_t scale);
eccles_gui_status_t eccles_gui_view3d_set_camera(eccles_gui_t *gui, eccles_gui_obj_t obj, int32_t x, int32_t y, int32_t z,
                                                 uint16_t yaw, uint16_t pitch, int focal);
eccles_gui_status_t eccles_gui_view3d_set_light(eccles_gui_t *gui, eccles_gui_obj_t obj, int dx, int dy, int dz, uint8_t ambient);
eccles_gui_status_t eccles_gui_view3d_set_mode(eccles_gui_t *gui, eccles_gui_obj_t obj, int index, uint8_t mode);
eccles_gui_status_t eccles_gui_view3d_set_texture(eccles_gui_t *gui, eccles_gui_obj_t obj, int index, const eccles_gui_image_t *tex);
//call after you changed the scene through the pointer from eccles_gui_view3d_get (the setters above already do it)
void eccles_gui_view3d_changed(eccles_gui_t *gui, eccles_gui_obj_t obj);
#endif

//animation

#if ECCLES_GUI_HAS_ANIM && ECCLES_GUI_MAX_ANIMS > 0
//start an animation. one with the same (obj, prop, index) is replaced. needs eccles_gui_update() to be running
eccles_gui_status_t eccles_gui_anim_start(eccles_gui_t *gui, const eccles_gui_anim_t *anim);
//stop animations of a widget (prop 0 = all of its properties). the value stays where it is. returns how many were stopped
int eccles_gui_anim_cancel(eccles_gui_t *gui, eccles_gui_obj_t obj, uint8_t prop);
//stop the custom animations that use this target
int eccles_gui_anim_cancel_target(eccles_gui_t *gui, void *target);
int eccles_gui_anim_count(const eccles_gui_t *gui);
//the easing curve itself: t and the result are 0..1024 (the result can go past 1024 for OUT_BACK)
int32_t eccles_gui_ease(uint8_t ease, int32_t t);
#endif

#ifdef __cplusplus
}
#endif

#endif  //ECCLES_GUI_H
