//eccles_gui host example: every built in mesh in every shading mode, drawn with eccles_gui_draw_view3d inside a custom draw callback
//cc -std=c99 -O2 -Isrc -Iconfig examples/host/meshes.c src/eccles_gui.c -o meshes && ./meshes meshes.ppm [z]   (add z to use the z buffer)
#include "eccles_gui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define W 480
#define H 320
static uint16_t fb[W*H];
static void fl(void*u,int x,int y,int w,int h,const uint16_t*p){(void)u;for(int j=0;j<h;j++)memcpy(fb+(y+j)*W+x,p+j*w,(size_t)w*2);}
static uint16_t tex[64*64];
static eccles_gui_image_t timg={tex,64,64,0,0};
static eccles_gui_view3d_t sc[8];
static void draw(eccles_gui_dc_t*dc,const eccles_gui_area_t*a,void*u){(void)a;(void)u;
 for(int i=0;i<8;i++) eccles_gui_draw_view3d(dc,&sc[i],(i%4)*120,(i/4)*160,120,160);}
static void setup(int i,const eccles_gui_mesh_t*m,uint8_t mode,int yaw,int pitch){
 eccles_gui_view3d_defaults(&sc[i],160); sc[i].bg=ECCLES_GUI_HEX(0x202830);
 sc[i].cam_z=-5*4096; sc[i].focal=200; sc[i].obj[0].mesh=m; sc[i].obj[0].color=ECCLES_GUI_HEX(0x50A0FF);
 sc[i].obj[0].mode=mode; sc[i].obj[0].yaw=ECCLES_GUI_DEG(yaw); sc[i].obj[0].pitch=ECCLES_GUI_DEG(pitch); sc[i].obj[0].tex=&timg;
 sc[i].obj[0].flags|=ECCLES_GUI_3D_F_DOUBLE_SIDED*(m==&eccles_gui_mesh_card);}
int main(int argc,char**argv){
 for(int y=0;y<64;y++)for(int x=0;x<64;x++){int c=((x/8)+(y/8))&1; uint16_t col=c?0xFFFF:ECCLES_GUI_HEX(0xC03030); if(x<8&&y<8)col=ECCLES_GUI_HEX(0x00FF00); if(x>55&&y<8)col=ECCLES_GUI_HEX(0x0000FF); tex[y*64+x]=col;}
 setup(0,&eccles_gui_mesh_cube,ECCLES_GUI_3D_FLAT,35,25);
 setup(1,&eccles_gui_mesh_cube,ECCLES_GUI_3D_WIRE,35,25);
 setup(2,&eccles_gui_mesh_cube,ECCLES_GUI_3D_TEXTURED,35,25);
 setup(3,&eccles_gui_mesh_pyramid,ECCLES_GUI_3D_FLAT,35,25);
 setup(4,&eccles_gui_mesh_cylinder,ECCLES_GUI_3D_SMOOTH,0,60);
 setup(5,&eccles_gui_mesh_sphere,ECCLES_GUI_3D_FLAT,0,0);
 setup(6,&eccles_gui_mesh_card,ECCLES_GUI_3D_TEXTURED,50,0);
 setup(7,&eccles_gui_mesh_torus,ECCLES_GUI_3D_FLAT,0,60);
 static uint16_t band[W*8], zb[W*8]; static eccles_gui_t g; eccles_gui_desc_t d; memset(&d,0,sizeof d);
 d.width=W;d.height=H;d.buf=band;d.buf_pixels=sizeof band/2;d.flush=fl; if(argc>2) d.zbuf=zb;
 eccles_gui_init(&g,&d); eccles_gui_render_area(&g,0,0,W,H,draw,0);
 FILE*f=fopen(argv[1],"wb");fprintf(f,"P6\n%d %d\n255\n",W,H);
 for(int i=0;i<W*H;i++){unsigned p=fb[i];fputc(((p>>11)&31)<<3,f);fputc(((p>>5)&63)<<2,f);fputc((p&31)<<3,f);} fclose(f);
 eccles_gui_stats_t s=eccles_gui_get_stats(&g); printf("faces drawn %u dropped %u\n",s.faces_drawn,s.faces_dropped); return 0;}
