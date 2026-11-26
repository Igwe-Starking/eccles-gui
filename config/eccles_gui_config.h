/*
 * eccles_gui_config.h - build time options for eccles_gui
 *
 * every option is OFF / at its default until you change it here, or pass it on the compiler command line (-DNAME=value).
 * the header picks this file up by itself (it is found with __has_include), so just keep it on the include path.
 * every file that includes eccles_gui.h must see the same values.
 *
 * the numbers in the "cost" notes are for a 64 bit host with the default settings. on a 32 bit mcu pointers are half
 * the size so the context is smaller, sizes of the arrays below are the same.
 */

#ifndef ECCLES_GUI_CONFIG_H
#define ECCLES_GUI_CONFIG_H

//--- features you can remove (flash) ---------------------------------------------------------------------------------

//remove the 3d renderer, the view3d widget and the built in meshes (about -16 KB flash, -3.9 KB ram)
//#define ECCLES_GUI_NO_3D

//remove the widget tree, styles, theme, layout, touch input and animation targets. what is left is the band renderer
//and the drawing functions, you call eccles_gui_render_area() with your own draw callback (about -22 KB flash)
//#define ECCLES_GUI_NO_WIDGETS

//remove eccles_gui_anim_*. the context loses its animation table
//#define ECCLES_GUI_NO_ANIM

//no anti aliasing: edges of rounded rects, circles, lines and arcs become hard pixels. faster on slow cores
//#define ECCLES_GUI_NO_AA

//no soft shadows (they are the most expensive thing a style can ask for)
//#define ECCLES_GUI_NO_SHADOW

//do not link the built in 5x8 font. you must register your own font before any text is drawn (the 4 first font slots stay empty)
//#define ECCLES_GUI_NO_BUILTIN_FONT

//do not link the built in meshes (cube, pyramid, cylinder, sphere, torus, card, about 6 KB flash)
//#define ECCLES_GUI_NO_BUILTIN_MESHES

//--- pool sizes (ram) ------------------------------------------------------------------------------------------------
//nothing is allocated at run time. every pool is an array inside eccles_gui_t. when a pool is full the create call fails
//with ECCLES_GUI_ERR_NO_MEMORY and eccles_gui_last_error_str() tells you which pool

//widgets, root included. about 40 bytes each on a 32 bit chip
//#define ECCLES_GUI_MAX_WIDGETS 48

//style slots, 18 bytes each. the built in theme uses the first 8
//#define ECCLES_GUI_MAX_STYLES 16

//font slots, 8 bytes each. the built in font uses the first 4 (x1, x2, x3, x4)
//#define ECCLES_GUI_MAX_FONTS 8

//dirty rectangles kept per frame, 8 bytes each. when full, the two that cost the least to merge are merged
//#define ECCLES_GUI_MAX_DIRTY 8

//animations running at the same time, 44 bytes each
//#define ECCLES_GUI_MAX_ANIMS 8

//view3d widgets alive at the same time. each one holds a scene of ECCLES_GUI_3D_MAX_OBJECTS meshes (176 bytes each with 4 meshes)
//#define ECCLES_GUI_MAX_VIEWS3D 2

//meshes per 3d scene (1..8)
//#define ECCLES_GUI_3D_MAX_OBJECTS 4

//projected vertices / visible faces cached per frame. one shared cache for the whole context: 8 bytes per vertex and 8 per face
//the built in torus needs 96 vertices and 192 faces, the sphere 42 and 80, the cube 24 and 12.
//faces that do not fit are dropped (counted in eccles_gui_get_stats().faces_dropped)
//#define ECCLES_GUI_3D_MAX_VERTS 192
//#define ECCLES_GUI_3D_MAX_FACES 256

//--- 3d options ------------------------------------------------------------------------------------------------------

//0 compiles out the depth test. without a z buffer (desc.zbuf = NULL) the renderer sorts faces back to front (painter's
//algorithm), which is right for single convex objects and cheaper in ram. with desc.zbuf any scene is right
//#define ECCLES_GUI_3D_ZBUFFER 1

//0 compiles out textured faces (affine mapping)
//#define ECCLES_GUI_3D_TEXTURES 1

//--- input -----------------------------------------------------------------------------------------------------------

//pixels the finger may move and still count as a tap, a switch toggle or a long press
//#define ECCLES_GUI_TOUCH_SLOP 10

//pixels of travel for a swipe gesture, and the time for a long press. scale them with your panel density
//#define ECCLES_GUI_SWIPE_MIN 48
//#define ECCLES_GUI_LONG_PRESS_MS 500

//--- display ---------------------------------------------------------------------------------------------------------

//1 = every flushed pixel has its two bytes swapped, for spi panels that want the high byte first.
//(you can also set ECCLES_GUI_DESC_SWAP_BYTES in the descriptor at run time)
//#define ECCLES_GUI_SWAP_BYTES 0

//--- speed -----------------------------------------------------------------------------------------------------------

//1 puts the hot pixel loops (spans, blends, triangle rasteriser, glyph blits) in internal ram. code normally runs from flash
//through a cache; keeping the loops that run for every pixel in iram avoids cache misses, worth trying when the ui is
//your slowest task (measure it on your board). costs a few KB of iram. set ECCLES_GUI_IRAM_ATTR yourself for chips the
//header does not know
//#define ECCLES_GUI_EXEC_IRAM 0

//--- debug -----------------------------------------------------------------------------------------------------------

//print misuse (bad handles, full pools) to the console
//#define ECCLES_GUI_DEBUG
//and where to print it, if you do not want printf
//#define ECCLES_GUI_LOG_LINE(msg) my_log(msg)

#endif  //ECCLES_GUI_CONFIG_H
