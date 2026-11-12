/*
 * The renderer's pieces shared by gfx.c and the microcodes it runs that
 * aren't F3DEX2 (gfx_f5.c, gfx_dkr.c): those do their own RSP work (matrices,
 * vertices, display list flow) and hand gfx.c clip-space vertices,
 * triangles and the RDP commands.
 */
#ifndef GFX_INTERNAL_H
#define GFX_INTERNAL_H

#include <stdint.h>

#include "n64/ultratypes.h"
#include "n64/gbi.h"

#define MAX_VERTS 64

typedef struct {
    float x, y, z, w;
    float r, g, b, a;
    float u, v; // texture coords, 10.5 texels (after the G_TEXTURE scale)
    u8 clip; // outside x/y planes (bits 0-3); CLIP_NEAR: in front of the near plane
} RspVtx;
#define CLIP_NEAR 0x10

/* Vertex buffer slot i (mod MAX_VERTS) */
RspVtx* gfx_vtx(int i);
/* After filling x, y, z, w (clip space, GL's y-up) of a vertex: the
   widescreen field of view and the clip flags */
void gfx_finish_vtx(RspVtx* d);
/* A triangle of vertex buffer slots (culling by the geometry mode) */
void gfx_tri(int i0, int i1, int i2);
/* The 3D viewport in N64 screen pixels */
void gfx_set_viewport(float x, float y, float w, float h);
/* The current geometry mode (F3DEX2 bits) */
u32 gfx_geometry(void);
/* The primitive color as last set (RGBA) and the other mode's low word */
u32 gfx_prim_color(void);
u32 gfx_othermode_l(void);
/* S30GfxConfig.rdram */
u8* gfx_rdram(void);
/* This frame is being traced (<PREFIX>_GFXTRACE, SIGUSR2) */
int gfx_tracing(void);
/* An F3DEX2 command that isn't display list flow (see gfx.c); returns the
   number of 8-byte words after dl it consumed */
int gfx_command(const Gfx* dl, u32 w0, u32 w1);
/* The screen side of the 2D element being drawn (0 none, 1 left, 2 right):
   in widescreen its rectangles move out to that edge */
void gfx_set_rect_side(int side);
/* G_TEXRECT / G_TEXRECTFLIP with its RDPHALF words */
void gfx_texrect(u32 w0, u32 w1, u32 h1, u32 h2, int flip);
/* An F3DEX command as F3DEX2 (0: nothing to do) */
int gfx_f3dex_to_f3dex2(u32* pw0, u32* pw1);

/* The fog factor (0-1) of a vertex in clip space, from G_MW_FOG */
float gfx_fog(const RspVtx* d);

/* gfx_f5.c: one Factor 5 display list (the task's data_ptr in RDRAM) */
void gfx_f5_walk(void* displayList);
/* gfx_dkr.c: one F3DDKR display list */
void gfx_dkr_walk(void* displayList);

#endif
