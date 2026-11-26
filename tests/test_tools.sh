#!/bin/sh
# runs the three converters on sample data, compiles what they produce against the engine, renders it and checks the picture
# needs python3 + pillow. the font test is skipped when no truetype font is found
set -e
cd "$(dirname "$0")/.."
B=build/tools_test
mkdir -p $B
python3 - <<'PY'
from PIL import Image, ImageDraw
im = Image.new("RGBA", (24, 24), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
d.ellipse((2, 2, 21, 21), fill=(255, 255, 255, 255))
d.rectangle((10, 10, 13, 20), fill=(0, 0, 0, 0))
im.save("build/tools_test/icon.png")
open("build/tools_test/pyramid.obj", "w").write("""v 0 1 0
v -1 -1 -1
v 1 -1 -1
v 1 -1 1
v -1 -1 1
vt 0.5 1
vt 0 0
vt 1 0
f 1/1 2/2 3/3
f 1/1 3/2 4/3
f 1/1 4/2 5/3
f 1/1 5/2 2/3
f 2 5 4 3
""")
PY
for f in rgb565 key a8 a4 a1; do python3 tools/png2eccles.py $B/icon.png --name icon_$f --format $f > $B/icon_$f.c; done
python3 tools/obj2mesh.py $B/pyramid.obj --name obj_pyr > $B/obj_pyr.c
python3 tools/obj2mesh.py $B/pyramid.obj --name obj_pyr_flat --flat > $B/obj_pyr_flat.c
FONT=""
for f in /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf /usr/share/fonts/TTF/DejaVuSans.ttf /Library/Fonts/Arial.ttf /System/Library/Fonts/Supplemental/Arial.ttf; do
  if [ -f "$f" ]; then FONT=$f; break; fi
done
FONTSRC=""
if [ -n "$FONT" ]; then
  python3 tools/ttf2eccles_font.py "$FONT" --size 20 --bpp 4 --symbols "°" --name font_t4 > $B/font_t4.c
  python3 tools/ttf2eccles_font.py "$FONT" --size 20 --bpp 8 --name font_t8 > $B/font_t8.c
  python3 tools/ttf2eccles_font.py "$FONT" --size 16 --bpp 1 --name font_t1 > $B/font_t1.c
  FONTSRC="$B/font_t4.c $B/font_t8.c $B/font_t1.c"
  FONTDEF=-DHAVE_FONT
else
  echo "(no truetype font found, skipping the font converter test)"
  FONTDEF=
fi
cat > $B/check.c <<'CSRC'
#include "eccles_gui.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
extern const eccles_gui_image_t icon_rgb565, icon_key, icon_a8, icon_a4, icon_a1;
extern const eccles_gui_mesh_t obj_pyr, obj_pyr_flat;
#ifdef HAVE_FONT
extern const eccles_gui_font_t font_t4, font_t8, font_t1;
#endif
#define W 220
#define H 120
static uint16_t fb[W * H], band[W * 8];
static void fl(void *u, int x, int y, int w, int h, const uint16_t *p) { (void)u; for (int j = 0; j < h; j++) memcpy(fb + (y + j) * W + x, p + j * w, (size_t)w * 2); }
static eccles_gui_view3d_t scene, scene2;
static int fid[3];
static void draw(eccles_gui_dc_t *dc, const eccles_gui_area_t *a, void *u) {
    (void)a; (void)u;
    eccles_gui_draw_fill(dc, 0, 0, W, H, 0x2104, 255);
    eccles_gui_draw_image(dc, 4, 4, &icon_rgb565, 0, 255);
    eccles_gui_draw_image(dc, 32, 4, &icon_key, 0, 255);
    eccles_gui_draw_image(dc, 60, 4, &icon_a8, 0x07FF, 255);
    eccles_gui_draw_image(dc, 88, 4, &icon_a4, 0x07FF, 255);
    eccles_gui_draw_image(dc, 116, 4, &icon_a1, 0x07FF, 255);
#ifdef HAVE_FONT
    eccles_gui_draw_text(dc, 4, 34, (uint8_t)fid[0], "Hello W\xC3\xB6rld 25\xC2\xB0" "C", 0xFFFF, 255);
    eccles_gui_draw_text(dc, 4, 58, (uint8_t)fid[1], "Anti-aliased 8 bit", 0xFFE0, 255);
    eccles_gui_draw_text(dc, 4, 80, (uint8_t)fid[2], "1 bit font", 0xF81F, 255);
#endif
    eccles_gui_draw_view3d(dc, &scene, 150, 0, 70, 60);
    eccles_gui_draw_view3d(dc, &scene2, 150, 60, 70, 60);
}
int main(int argc, char **argv) {
    static eccles_gui_t g;
    eccles_gui_desc_t d; memset(&d, 0, sizeof d);
    d.width = W; d.height = H; d.buf = band; d.buf_pixels = sizeof band / 2; d.flush = fl;
    if (eccles_gui_init(&g, &d)) return 1;
#ifdef HAVE_FONT
    fid[0] = eccles_gui_font_register(&g, &font_t4, 1);
    fid[1] = eccles_gui_font_register(&g, &font_t8, 1);
    fid[2] = eccles_gui_font_register(&g, &font_t1, 1);
    if (fid[0] < 0 || fid[1] < 0 || fid[2] < 0) return 2;
    if (eccles_gui_text_width(&g, (uint8_t)fid[0], "Hello") < 30) return 3;
    if (eccles_gui_text_width(&g, (uint8_t)fid[0], "\xC2\xB0") < 3) return 4;  /* the degree sign made it into the font */
#endif
    eccles_gui_view3d_defaults(&scene, 60); scene.focal = 60; scene.cam_z = -4096 * 4; scene.bg = 0x1082;
    scene.obj[0].mesh = &obj_pyr; scene.obj[0].color = 0xFD20; scene.obj[0].yaw = ECCLES_GUI_DEG(30); scene.obj[0].pitch = ECCLES_GUI_DEG(-20);
    eccles_gui_view3d_defaults(&scene2, 60); scene2.focal = 60; scene2.cam_z = -4096 * 4; scene2.bg = 0x1082;
    scene2.obj[0].mesh = &obj_pyr_flat; scene2.obj[0].color = 0x07E0; scene2.obj[0].yaw = ECCLES_GUI_DEG(210); scene2.obj[0].pitch = ECCLES_GUI_DEG(-20);
    /* both converted meshes must satisfy the winding rule: cross(b-a, c-a) points along the stored normal */
    const eccles_gui_mesh_t *ms[2] = {&obj_pyr, &obj_pyr_flat};
    for (int k = 0; k < 2; k++) for (int f = 0; f < ms[k]->nfaces; f++) {
        const int16_t *a = &ms[k]->verts[ms[k]->faces[f*3]*3], *b = &ms[k]->verts[ms[k]->faces[f*3+1]*3], *c = &ms[k]->verts[ms[k]->faces[f*3+2]*3];
        long e1[3] = {b[0]-a[0], b[1]-a[1], b[2]-a[2]}, e2[3] = {c[0]-a[0], c[1]-a[1], c[2]-a[2]};
        long cr[3] = {e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0]};
        long dot = cr[0]*ms[k]->normals[f*3] + cr[1]*ms[k]->normals[f*3+1] + cr[2]*ms[k]->normals[f*3+2];
        if (dot <= 0) return 5;
    }
    eccles_gui_render_area(&g, 0, 0, W, H, draw, NULL);
    int lit[5] = {0};
    const int xs[5] = {4, 32, 60, 88, 116};
    for (int i = 0; i < 5; i++) for (int y = 4; y < 28; y++) for (int x = xs[i]; x < xs[i] + 24; x++) if (fb[y * W + x] != 0x2104) lit[i]++;
    printf("icon pixels drawn: rgb565 %d key %d a8 %d a4 %d a1 %d\n", lit[0], lit[1], lit[2], lit[3], lit[4]);
    for (int i = 0; i < 5; i++) if (lit[i] < 200) return 6;
    if (lit[0] < lit[1]) return 7;  /* the key image skips the transparent corners, so it draws fewer pixels than the opaque one */
    int text = 0;
    for (int y = 34; y < 100; y++) for (int x = 0; x < 148; x++) if (fb[y * W + x] != 0x2104) text++;
    int p1 = 0, p2 = 0;
    for (int y = 0; y < 60; y++) for (int x = 150; x < 220; x++) if (fb[y * W + x] != 0x1082) p1++;
    for (int y = 60; y < 120; y++) for (int x = 150; x < 220; x++) if (fb[y * W + x] != 0x1082) p2++;
    printf("text pixels %d, pyramid (smooth) %d, pyramid (flat) %d\n", text, p1, p2);
#ifdef HAVE_FONT
    if (text < 400) return 8;
#endif
    if (p1 < 300 || p2 < 300) return 9;
    if (argc > 1) { FILE *f = fopen(argv[1], "wb"); fprintf(f, "P6\n%d %d\n255\n", W, H); for (int i = 0; i < W * H; i++) { unsigned p = fb[i]; fputc(((p>>11)&31)<<3, f); fputc(((p>>5)&63)<<2, f); fputc((p&31)<<3, f); } fclose(f); }
    return 0;
}
CSRC
cc -std=c99 -Wall -Wextra -Isrc -Iconfig $FONTDEF $B/check.c $B/icon_*.c $B/obj_pyr.c $B/obj_pyr_flat.c $FONTSRC src/eccles_gui.c -o $B/check
$B/check $B/check.ppm
echo "tools ok"
