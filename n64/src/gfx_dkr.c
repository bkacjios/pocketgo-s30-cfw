/*
 * Rare's F3DDKR microcode (Diddy Kong Racing), done on the CPU in front of
 * gfx.c's RDP. It is Fast3D with these differences, as GLideN64 implements
 * them (F3DDKR.cpp, gSP.cpp):
 *
 * - G_MTX (0x01) loads a whole model-view-projection matrix (the game
 *   multiplies on the CPU) into one of four slots, (w0 >> 22) & 3, which
 *   becomes the current one; G_MOVEWORD 0x0A selects a slot, (w1 >> 6) & 3.
 * - G_VTX (0x04) loads n = ((w0 >> 19) & 0x1F) + 1 ten-byte vertices (s16
 *   x, y, z; u8 r, g, b, a), at the start of the buffer or, with bit 16
 *   (append), after the last load.
 * - Billboards (G_MOVEWORD 0x02, w1 & 1): vertices loaded while on are in
 *   sprite space, added to vertex 0's clip-space position (the anchor, loaded
 *   before, through the camera); an appending load then starts at vertex 1.
 * - G_TRIN (0x05) draws (w0 & 0xFFFF) / 16 triangles from w1, 16 bytes each:
 *   flags (0x40: no culling), the vertex indices v0, v1, v2, then each
 *   corner's s, t (s10.5 texels). After it the next load starts at 0 again.
 * - G_DMADL (0x07) runs (w0 >> 16) & 0xFF commands at w1, then returns.
 *
 * The game's vertices and triangles are native structs (its assets are put
 * in this byte order as they are loaded); everything else goes to gfx.c as
 * the F3DEX command it is.
 */
#include <stdio.h>
#include <string.h>

#define F3DEX_GBI_2
#include "n64/ultratypes.h"
#include "n64/gbi.h"
#include "gfx_internal.h"
#include "s30_gfx.h"

#define DL_DEPTH 16
#define DKR_VERTS 56     // the microcode's 32, with room for an append past it
#define TRI_SLOT DKR_VERTS // three slots after the vertices: the triangle being drawn

typedef struct {
    s16 x, y, z;
    u8 r, g, b, a;
} DkrVtx;

typedef struct {
    u8 flags, v0, v1, v2;
    s16 s0, t0, s1, t1, s2, t2;
} DkrTri;

static struct {
    float mtx[4][4][4];
    int cur;
    int billboard;
    int next;          // where the next load goes
    float anchor[4];   // vertex 0 in clip space, before gfx_finish_vtx
} dkr;

/* Mtx: s15.16, integer halves then fraction halves, each where its index puts
   it (16-bit elements) */
static void load_mtx(float m[4][4], const void* mtx) {
    const u16* i = mtx;
    const u16* f = i + 16;
    int e;

    for (e = 0; e < 16; e++) {
        m[e / 4][e % 4] = (s32)(((u32)i[e] << 16) | f[e]) / 65536.0f;
    }
}

static void handle_mtx(u32 w0, u32 w1) {
    const void* m = pc_resolve_addr(w1);

    if (m == NULL || (w0 & 0xFFFF) != 64) {
        return;
    }
    dkr.cur = (w0 >> 22) & 3;
    load_mtx(dkr.mtx[dkr.cur], m);
}

static void handle_vtx(u32 w0, u32 w1) {
    const DkrVtx* src = pc_resolve_addr(w1);
    int n = ((w0 >> 19) & 0x1F) + 1;
    int dst, i;
    float(*m)[4] = dkr.mtx[dkr.cur];

    if (w0 & 0x00010000) {
        if (dkr.billboard) {
            dkr.next = 1;
        }
    } else {
        dkr.next = 0;
    }
    dst = dkr.next + ((w0 >> 9) & 0x1F);
    if (src == NULL || dst + n > DKR_VERTS) {
        return;
    }
    for (i = 0; i < n; i++) {
        const DkrVtx* v = &src[i];
        RspVtx* d = gfx_vtx(dst + i);
        float x = v->x, y = v->y, z = v->z;
        float c[4];

        c[0] = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
        c[1] = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
        c[2] = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
        c[3] = x * m[0][3] + y * m[1][3] + z * m[2][3] + m[3][3];
        if (dkr.billboard) {
            c[0] += dkr.anchor[0];
            c[1] += dkr.anchor[1];
            c[2] += dkr.anchor[2];
            c[3] += dkr.anchor[3];
        }
        if (dst + i == 0) {
            memcpy(dkr.anchor, c, sizeof(c));
        }
        d->x = c[0];
        d->y = c[1];
        d->z = c[2];
        d->w = c[3];
        gfx_finish_vtx(d);
        d->r = v->r / 255.0f;
        d->g = v->g / 255.0f;
        d->b = v->b / 255.0f;
        d->a = (gfx_geometry() & G_FOG) ? gfx_fog(d) : v->a / 255.0f;
        d->u = 0;
        d->v = 0;
    }
    dkr.next += n;
}

/* A corner of the triangle being drawn: the vertex with its texcoords */
static void tri_corner(int slot, int vi, s16 s, s16 t) {
    RspVtx* d = gfx_vtx(TRI_SLOT + slot);

    *d = *gfx_vtx(vi % DKR_VERTS);
    d->u = s;
    d->v = t;
}

static void set_cull(int cull) {
    u32 on = gfx_geometry() & G_CULL_BACK;
    Gfx g = { { 0, 0 } };

    if (cull && !on) {
        gfx_command(&g, ((u32)G_GEOMETRYMODE << 24) | 0xFFFFFF, G_CULL_BACK);
    } else if (!cull && (gfx_geometry() & G_CULL_BOTH)) {
        gfx_command(&g, ((u32)G_GEOMETRYMODE << 24) | (~G_CULL_BOTH & 0xFFFFFF), 0);
    }
}

/* Texturing is G_TRIN's bit 16 (DKR's display lists leave G_TEXTURE off):
   the render tile, texcoords as they are */
static void set_texture(int on) {
    Gfx g = { { 0, 0 } };

    gfx_command(&g, ((u32)G_TEXTURE << 24) | (on ? 2 : 0), 0xFFFFFFFF); // a draw ends only on a change
}

static void handle_trin(u32 w0, u32 w1) {
    const DkrTri* t = pc_resolve_addr(w1);
    int n = (w0 >> 4) & 0xFFF, i;

    set_texture((w0 >> 16) & 1);
    if (t != NULL) {
        for (i = 0; i < n; i++, t++) {
            set_cull(!(t->flags & 0x40));
            tri_corner(0, t->v0, t->s0, t->t0);
            tri_corner(1, t->v1, t->s1, t->t1);
            tri_corner(2, t->v2, t->s2, t->t2);
            gfx_tri(TRI_SLOT, TRI_SLOT + 1, TRI_SLOT + 2);
        }
    }
    dkr.next = 0;
}

/* The last display list calls (where from, the command), for reports */
static struct {
    const Gfx* from;
    u32 w0, w1;
} sCalls[16];
static int sNumCalls;

static void note_call(const Gfx* from, u32 w0, u32 w1) {
    int i = sNumCalls++ % 16;

    sCalls[i].from = from;
    sCalls[i].w0 = w0;
    sCalls[i].w1 = w1;
}

/* max: the number of commands to run (G_DMADL), or -1 to the G_ENDDL */
static void walk(const Gfx* dl, int depth, int max) {
    if (dl == NULL || depth > DL_DEPTH || ((uintptr_t)dl & 3)) {
        return;
    }
    for (; max != 0; dl++, max--) {
        u32 w0 = dl->words.w0, w1 = dl->words.w1;
        u8 cmd = w0 >> 24;

        if (gfx_tracing()) {
            printf("%*s%08x %08x\n", depth * 2, "", (unsigned)w0, (unsigned)w1);
        }
        switch (cmd) {
            case 0x01: // G_MTX
                handle_mtx(w0, w1);
                continue;
            case 0x04: // G_VTX
                handle_vtx(w0, w1);
                continue;
            case 0x05: // G_TRIN
                handle_trin(w0, w1);
                continue;
            case 0x06: // G_DL
                note_call(dl, w0, w1);
                walk(pc_resolve_addr(w1), depth + 1, -1);
                if ((w0 >> 16) & 0xFF) {
                    return; // branch, no return
                }
                continue;
            case 0x07: // G_DMADL
                note_call(dl, w0, w1);
                walk(pc_resolve_addr(w1), depth + 1, (w0 >> 16) & 0xFF);
                continue;
            case 0xB8: // G_ENDDL
                return;
            case 0xBC: // G_MOVEWORD
                if ((w0 & 0xFF) == 0x02) {
                    dkr.billboard = w1 & 1;
                    continue;
                }
                if ((w0 & 0xFF) == 0x0A) {
                    dkr.cur = (w1 >> 6) & 3;
                    continue;
                }
                break;
            case 0xBF: // G_TRI1: not in F3DDKR's display lists
                continue;
        }
        if (cmd == 0xFD && w1 < 0x01000000) {
            // A texture image below 16 MiB: report where (a few times)
            static int reported;

            if (reported++ < 4) {
                int k;

                printf("gfx_dkr: G_SETTIMG %08x %08x at %p (depth %d, %d left); before:", (unsigned)w0, (unsigned)w1,
                       (const void*)dl, depth, max);
                for (k = -6; k < 0; k++) {
                    printf(" %08x:%08x", (unsigned)dl[k].words.w0, (unsigned)dl[k].words.w1);
                }
                printf("\ngfx_dkr: last calls:");
                for (k = 0; k < 16 && k < sNumCalls; k++) {
                    int c = (sNumCalls - 1 - k) % 16;
                    printf(" %p:%08x:%08x", (const void*)sCalls[c].from, (unsigned)sCalls[c].w0, (unsigned)sCalls[c].w1);
                }
                printf("\n");
            }
            if (depth > 0) {
                return; // not a display list: give up on this one
            }
            continue;
        }
        if (!gfx_f3dex_to_f3dex2(&w0, &w1)) {
            continue;
        }
        {
            // The command as F3DEX2, with the words after it as they are
            // (texture rectangles read their RDPHALF words)
            Gfx g[3];
            int k, used;

            g[0].words.w0 = w0;
            g[0].words.w1 = w1;
            for (k = 1; k < 3; k++) {
                g[k] = dl[k];
            }
            used = gfx_command(g, w0, w1);
            dl += used;
            if (max > 0) {
                max = max > used ? max - used : 1;
            }
        }
    }
}

void gfx_dkr_walk(void* displayList) {
    int i, j;

    memset(&dkr, 0, sizeof(dkr));
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            dkr.mtx[i][j][j] = 1.0f;
        }
    }
    walk(displayList, 0, -1);
}
