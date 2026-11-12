/*
 * Factor 5's RSP microcode (Rogue Squadron), done on the CPU in front of
 * gfx.c's RDP. Display lists are 0x108-byte chunks: an 8-byte header (next,
 * prev) then commands, continuing in the next chunk at its end or on 0xB5.
 * Models come as batches of 8-byte vertices and faces that carry their own
 * vertex colors and texcoords; terrain comes as 40-byte tile records the
 * microcode expands into a grid. The game's notes on all of it are in
 * RogueSquadron64Recomp's docs/reference (factor5-gbi.md, f5-model-dl-spec.md,
 * f5-terrain-tile.md); this follows their RT64 implementation.
 *
 * RDRAM is the static recompilation's: native u32 words, so the N64 byte at
 * A is at A ^ 3 and the halfword at A ^ 2.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#define F3DEX_GBI_2
#include "n64/ultratypes.h"
#include "n64/gbi.h"
#include "gfx_internal.h"
#include "s30_gfx.h"

#define RDRAM_SIZE 0x800000u
#define CHUNK_SIZE 0x108u
#define STACK_DEPTH 16
#define MAX_HOPS 4096
#define MAX_COMMANDS 65536
#define F5_CACHE 64

static u8* sRam;

/* <PREFIX>_F5STATS: one line per 100 display lists */
static struct {
    u32 lists, cmds, faces, verts, tiles, hops, calls, endTop, endLink, endCap, unknown, cimgW;
} sF5;
static int sF5StatsOn = -1;

static inline u32 rd32(u32 a) {
    return *(const u32*)(sRam + (a & 0xFFFFFC));
}
static inline s16 rd16s(u32 a) {
    return *(const s16*)(sRam + ((a & 0xFFFFFE) ^ 2));
}
static inline u16 rd16(u32 a) {
    return *(const u16*)(sRam + ((a & 0xFFFFFE) ^ 2));
}
static inline u8 rd8(u32 a) {
    return sRam[(a & 0xFFFFFF) ^ 3];
}
static inline int in_ram(u32 a, u32 n) {
    a &= 0xFFFFFF;
    return a + n <= RDRAM_SIZE;
}

/* Microcode state; each task starts from it reset */
static struct {
    float mv[4][4], p[4][4], mvp[4][4];
    int mvpDirty;
    float fogM, fogO;       // fog alpha = clamp(z/w * M + O)
    s32 tcScale[2];         // 03 82: 16.16 per-face texcoord scale (s, t)
    u16 texScale[2];        // G_TEXTURE
    u32 colorBuf;           // 02: per-vertex RGBA
    // The vertex cache (04): clip space with GL's y up, the batch's colors and fog
    float pos[F5_CACHE][4];
    float col[F5_CACHE][4];
    float fog[F5_CACHE];
    u32 cacheCount;
    // The viewport: the stream's own (03 80) for the rest of the task, or else
    // the scissor rectangle, which the microcode maps to (the radar's pass)
    int vpFromStream;
    u32 vpKey;
    u32 scissor[2];
} f5;

/* Before a transform: the viewport from the scissor, unless the stream set one */
static void ensure_viewport(void) {
    u32 ulx = (f5.scissor[0] >> 12) & 0xFFF, uly = f5.scissor[0] & 0xFFF;
    u32 lrx = (f5.scissor[1] >> 12) & 0xFFF, lry = f5.scissor[1] & 0xFFF;
    int w = (int)(lrx - ulx) / 4, h = (int)(lry - uly) / 4;
    u32 key;

    if (f5.vpFromStream) {
        return;
    }
    if (w <= 0 || w > 1024 || h <= 0 || h > 1024) {
        ulx = uly = 0;
        w = 320;
        h = 240;
    }
    key = (ulx << 20) ^ (uly << 10) ^ ((u32)w << 16) ^ (u32)h;
    if (key != f5.vpKey) {
        f5.vpKey = key;
        gfx_set_viewport(ulx / 4.0f, uly / 4.0f, (float)w, (float)h);
    }
}

static void mtx_mul(float r[4][4], float a[4][4], float b[4][4]) {
    float t[4][4];
    int i, j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    memcpy(r, t, sizeof(t));
}

static void update_mvp(void) {
    if (f5.mvpDirty) {
        mtx_mul(f5.mvp, f5.mv, f5.p);
        f5.mvpDirty = 0;
    }
}

/* Object position -> clip space (y negated: the microcode's NDC is y-down)
   and the fog alpha */
static void transform(float x, float y, float z, float out[4], float* fog) {
    float(*m)[4] = f5.mvp;
    float w;

    out[0] = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
    out[1] = -(x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1]);
    out[2] = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
    out[3] = x * m[0][3] + y * m[1][3] + z * m[2][3] + m[3][3];
    w = out[3];
    if (fabsf(w) < 0.001f) {
        w = 0.001f;
    }
    {
        float f = (w < 0.0f ? 32767.0f : out[2] / w) * f5.fogM + f5.fogO;
        *fog = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
    }
}

/* Fill gfx.c vertex slot i */
static void put_vertex(int i, const float pos[4], const float col[4], float fog, float u, float v) {
    RspVtx* d = gfx_vtx(i);

    d->x = pos[0];
    d->y = pos[1];
    d->z = pos[2];
    d->w = pos[3];
    d->r = col[0];
    d->g = col[1];
    d->b = col[2];
    d->a = (gfx_geometry() & G_FOG) ? fog : col[3];
    d->u = u;
    d->v = v;
    gfx_finish_vtx(d);
}

static void color_word(float c[4], u32 w) {
    c[0] = (w >> 24) / 255.0f;
    c[1] = ((w >> 16) & 0xFF) / 255.0f;
    c[2] = ((w >> 8) & 0xFF) / 255.0f;
    c[3] = (w & 0xFF) / 255.0f;
}

/* ---------------------------------------------------------------------------
 * State commands
 * ------------------------------------------------------------------------- */

/* 01: a 64-byte N64 Mtx; byte 1 bit 0: projection, else modelview */
static void op_matrix(u32 w0, u32 w1) {
    float m[4][4];
    int e;

    if (!in_ram(w1, 64)) {
        return;
    }
    for (e = 0; e < 16; e++) {
        u32 i = rd16(w1 + 2 * e), f = rd16(w1 + 32 + 2 * e);
        m[e / 4][e % 4] = (s32)((i << 16) | f) / 65536.0f;
    }
    memcpy(((w0 >> 16) & 1) ? f5.p : f5.mv, m, sizeof(m));
    f5.mvpDirty = 1;
    if (gfx_tracing()) {
        printf("f5 mtx %s @%06X: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f\n",
               ((w0 >> 16) & 1) ? "proj" : "mv", w1, m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3],
               m[2][0], m[2][1], m[2][2], m[2][3], m[3][0], m[3][1], m[3][2], m[3][3]);
    }
    ensure_viewport();
}

/* 03: byte 1 picks a DMEM slot, the next 16 bytes are its contents */
static void op_state(u32 w0, const u32* d) {
    u32 sub = (w0 >> 16) & 0xFF;

    if (sub == 0x80) {
        // Viewport: vscale x, y, z, pad then vtrans, 2-bit fixed
        float sx = (s16)(d[0] >> 16) / 4.0f, sy = fabsf((s16)d[0] / 4.0f);
        float tx = (s16)(d[2] >> 16) / 4.0f, ty = (s16)d[2] / 4.0f;

        gfx_set_viewport(tx - sx, ty - sy, 2.0f * sx, 2.0f * sy);
        f5.vpFromStream = 1;
        if (gfx_tracing()) {
            printf("f5 viewport scale %.2f,%.2f,%d trans %.2f,%.2f,%d\n", sx, (s16)d[0] / 4.0f, (s16)(d[1] >> 16), tx, ty,
                   (s16)(d[3] >> 16));
        }
    } else if (sub == 0x88) {
        f5.fogM = (s32)d[0] / 65536.0f;
        f5.fogO = (s32)d[1] / 65536.0f;
    } else if (sub == 0x82) {
        // Texcoord scale: 4 high halfwords (s, t, s, t) then the 4 low ones
        f5.tcScale[0] = (s32)((d[0] & 0xFFFF0000u) | (d[2] >> 16));
        f5.tcScale[1] = (s32)((d[0] << 16) | (d[2] & 0xFFFFu));
    }
}

/* 04: (w0 >> 10) & 0x3F vertices of x, y, z s16 (and padding), colored by
   the last 02 */
static void op_vertex(u32 w0, u32 w1) {
    u32 n = (w0 >> 10) & 0x3F, i;
    int haveColors = f5.colorBuf >= 0x80000000u && in_ram(f5.colorBuf, n * 4);

    if (n == 0 || !in_ram(w1, n * 8)) {
        return;
    }
    ensure_viewport();
    update_mvp();
    for (i = 0; i < n; i++) {
        u32 a = w1 + i * 8;

        transform(rd16s(a), rd16s(a + 2), rd16s(a + 4), f5.pos[i], &f5.fog[i]);
        color_word(f5.col[i], haveColors ? rd32(f5.colorBuf + i * 4) : 0xFFFFFFFFu);
    }
    f5.cacheCount = n;
}

/* ---------------------------------------------------------------------------
 * Faces
 * ------------------------------------------------------------------------- */

static s16 tc_apply(s16 raw, s32 scale) {
    s64 v = ((s64)raw * scale) >> 16;
    return (s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* A triangle or quad of cache vertices. Colors are per face: byte offsets
   into the 02 buffer; texcoords (when textured) follow the command as
   (s, t) halfword pairs, scaled by the 03 82 scale. */
static void emit_face(const u32* idx, int n, const u32* st, u32 colorWord) {
    u32 cofs[4] = { (colorWord >> 16) & 0xFF, (colorWord >> 8) & 0xFF, colorWord & 0xFF, colorWord >> 24 };
    int haveColors = f5.colorBuf >= 0x80000000u && in_ram(f5.colorBuf, 0x100);
    int k;

    for (k = 0; k < n; k++) {
        if (idx[k] >= f5.cacheCount) {
            return; // stale index
        }
    }
    if (gfx_tracing()) {
        printf("f5 face n%d idx %u %u %u %u cw %08X cbuf %08X st %08X %08X %08X %08X tc %08X %08X\n", n, idx[0], idx[1], idx[2],
               n == 4 ? idx[3] : 0, colorWord, f5.colorBuf, st ? st[0] : 0, st ? st[1] : 0, st ? st[2] : 0,
               st && n == 4 ? st[3] : 0, (u32)f5.tcScale[0], (u32)f5.tcScale[1]);
        for (k = 0; k < n; k++) {
            printf("   v%d pos %.1f %.1f %.1f %.1f vcol %.2f %.2f %.2f %.2f facecol %08X\n", k, f5.pos[idx[k]][0], f5.pos[idx[k]][1],
                   f5.pos[idx[k]][2], f5.pos[idx[k]][3], f5.col[idx[k]][0], f5.col[idx[k]][1], f5.col[idx[k]][2], f5.col[idx[k]][3],
                   haveColors ? rd32(f5.colorBuf + cofs[k]) : 0);
        }
    }
    for (k = 0; k < n; k++) {
        float col[4], u = 0.0f, v = 0.0f;

        if (haveColors) {
            color_word(col, rd32(f5.colorBuf + cofs[k]));
        } else {
            memcpy(col, f5.col[idx[k]], sizeof(col));
        }
        if (st != NULL) {
            u = (float)((tc_apply((s16)(st[k] >> 16), f5.tcScale[0]) * (s32)f5.texScale[0]) >> 16);
            v = (float)((tc_apply((s16)(st[k] & 0xFFFF), f5.tcScale[1]) * (s32)f5.texScale[1]) >> 16);
        }
        put_vertex(k, f5.pos[idx[k]], col, f5.fog[idx[k]], u, v);
    }
    gfx_tri(0, 1, 2);
    if (n == 4) {
        gfx_tri(0, 2, 3);
    }
}

/* BF / 08: triangle; B4 / 13: quad (4th vertex in w1 byte 0). w0 & 2:
   textured, 32 bytes, else 16. Returns the extra words consumed. */
static int op_face(const u32* c, int quad) {
    u32 w0 = c[0], w1 = c[1];
    u32 idx[4] = { ((w1 >> 16) & 0xFF) / 5u, ((w1 >> 8) & 0xFF) / 5u, (w1 & 0xFF) / 5u, (w1 >> 24) / 5u };
    int n = quad ? 4 : 3;

    if (w0 & 2) {
        u32 st[4] = { c[4], c[5], c[6], c[7] };
        emit_face(idx, n, st, c[2]);
        return 3;
    }
    emit_face(idx, n, NULL, c[2]);
    return 1;
}

/* BD: a camera-facing sprite (fire, smoke, explosions) of 24 bytes: w0 bits
   5-11 the center's cache slot (x 0x28), bit 0 swaps the axes, bits 1/2 flip
   S/T; w1 its color (drawn as PRIM); word 3 the half size (x, y), word 4 the
   texture extent (S, T, 10.5). Drawn translucent and depth tested. */
static void op_sprite(const u32* c) {
    u32 slot = ((c[0] >> 5) & 0x7F8u) / 0x28u;
    int swap = (c[0] & 1) != 0;
    s16 hx = (s16)(c[3] >> 16), hy = (s16)(c[3] & 0xFFFF);
    float texS = (float)(s16)(c[4] >> 16), texT = (float)(s16)(c[4] & 0xFFFF);
    static const float sgx[4] = { -1, 1, -1, 1 }, sgy[4] = { -1, -1, 1, 1 };
    float sx, sy;
    u32 savedPrim, savedL;
    int k;

    if (slot >= f5.cacheCount) {
        return;
    }
    if (swap) {
        s16 t = hx;
        hx = hy;
        hy = t;
    }
    if (hx <= 0 || hx > 4000) {
        hx = 40;
    }
    if (hy <= 0 || hy > 4000) {
        hy = 40;
    }
    update_mvp();
    // Object units to clip space along the screen axes: the modelview's scale
    // times the projection's (y negated, as the transform does)
    sx = sqrtf(f5.mv[0][0] * f5.mv[0][0] + f5.mv[0][1] * f5.mv[0][1] + f5.mv[0][2] * f5.mv[0][2]) * f5.p[0][0];
    sy = sqrtf(f5.mv[1][0] * f5.mv[1][0] + f5.mv[1][1] * f5.mv[1][1] + f5.mv[1][2] * f5.mv[1][2]) * f5.p[1][1];
    for (k = 0; k < 4; k++) {
        static const float white[4] = { 1, 1, 1, 1 };
        float pos[4], sf = (sgx[k] + 1.0f) * 0.5f, tf = (sgy[k] + 1.0f) * 0.5f, u, v;

        memcpy(pos, f5.pos[slot], sizeof(pos));
        pos[0] += sgx[k] * hx * sx;
        pos[1] -= sgy[k] * hy * sy;
        if (swap) {
            float t = sf;
            sf = tf;
            tf = t;
        }
        if (c[0] & 2) {
            sf = 1.0f - sf;
        }
        if (c[0] & 4) {
            tf = 1.0f - tf;
        }
        // The microcode's texrect starts half a texel (0x10) early
        u = (float)(((s32)(sf * texS - 16.0f) * (s32)f5.texScale[0]) >> 16);
        v = (float)(((s32)(tf * texT - 16.0f) * (s32)f5.texScale[1]) >> 16);
        put_vertex(k, pos, white, f5.fog[slot], u, v);
    }
    savedPrim = gfx_prim_color();
    savedL = gfx_othermode_l();
    {
        Gfx g;
        g.words.w0 = (u32)G_SETPRIMCOLOR << 24;
        g.words.w1 = c[1];
        gfx_command(&g, g.words.w0, g.words.w1);
        g.words.w0 = ((u32)G_SETOTHERMODE_L << 24) | (0 << 8) | 31;
        g.words.w1 = 0x00504A50u;
        gfx_command(&g, g.words.w0, g.words.w1);
    }
    gfx_tri(0, 3, 2);
    gfx_tri(0, 1, 3);
    {
        Gfx g;
        g.words.w0 = (u32)G_SETPRIMCOLOR << 24;
        g.words.w1 = savedPrim;
        gfx_command(&g, g.words.w0, g.words.w1);
        g.words.w0 = ((u32)G_SETOTHERMODE_L << 24) | (0 << 8) | 31;
        g.words.w1 = savedL;
        gfx_command(&g, g.words.w0, g.words.w1);
    }
}

/* ---------------------------------------------------------------------------
 * Terrain tiles (05 05 ..): see f5-terrain-tile.md
 * ------------------------------------------------------------------------- */

static s32 clamp16(s64 v) {
    return (s32)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static s32 morph(s64 acc, s32 h, u32 w) {
    s64 hi = clamp16(acc >> 16), lo = acc & 0xFFFF;
    s64 a2 = ((lo * (s64)w) >> 16) + hi * (s64)w;

    a2 += (s64)h * (s64)((0x10000u - w) & 0xFFFFu);
    return clamp16(a2 >> 16);
}

/* Height (channel 0) and RGBA (1-4) of the M x M grid, [row j along z][column i along x] */
static s32 sGrid[5][5][5];
static int sGridM;

static s32* edge_point(int e, int k, int ch) {
    int M = sGridM;

    return e == 0 ? &sGrid[0][k][ch] : e == 1 ? &sGrid[M - 1][k][ch] : e == 2 ? &sGrid[k][0][ch] : &sGrid[k][M - 1][ch];
}

/* An LOD seam: edge e (0 z-min row, 1 z-max row, 2 x-min column, 3 x-max
   column) against a neighbour of LOD nb, morph weight w */
static void seam(int e, int nb, int s, u32 w) {
    int M = sGridM, k, ch;

    for (ch = 0; ch < 5; ch++) {
        if (nb != s) {
            for (k = 1; k < M - 1; k += 2) {
                *edge_point(e, k, ch) = (*edge_point(e, k - 1, ch) + *edge_point(e, k + 1, ch)) >> 1;
            }
            if (w != 0 && M >= 5) {
                for (k = 1; k < M - 1; k++) {
                    s64 f = (s64)(k & 3) << 14;
                    int k0 = k & ~3;

                    if ((k & 3) == 0) {
                        continue;
                    }
                    *edge_point(e, k, ch) = morph((s64)*edge_point(e, k0, ch) * (0x10000 - f) + (s64)*edge_point(e, k0 + 4, ch) * f,
                                                  *edge_point(e, k, ch), w);
                }
            }
        } else if (w != 0) {
            for (k = 1; k < M - 1; k += 2) {
                *edge_point(e, k, ch) = morph((s64)*edge_point(e, k - 1, ch) * 0x8000 + (s64)*edge_point(e, k + 1, ch) * 0x8000,
                                              *edge_point(e, k, ch), w);
            }
        }
    }
}

/* Vertex slot of grid point (i, j) */
#define GRID_SLOT(i, j) ((j) * 5 + (i))

static void tile_vertex(int slot, float x, float y, float z, const float col[4], float u, float v) {
    float pos[4], fog;

    transform(x, y, z, pos, &fog);
    put_vertex(slot, pos, col, fog, u, v);
}

/* 05 05 00: heightfield grid (s8 heights and RGBA colors from RDRAM) */
static void tile_grid(const u32* rec) {
    u32 hptr = rec[2] & 0xFFFFFF, cptr = rec[3] & 0xFFFFFF;
    int N = (int)((rec[0] >> 16) & 0xFF), s = (int)(rec[0] & 0xFF) & 7;
    int L = (int)((rec[0] >> 12) & 0xF);
    int M = ((N - 1) >> s) + 1, stride = 1 << s;
    s32 x = (s16)(rec[8] >> 16), y = (s32)(s16)rec[8] << 4, z = (s16)(rec[9] >> 16), size = (s16)rec[9];
    s32 step = (s16)(rec[7] & 0xFFFF);
    u32 nbw = rec[1];
    u32 wInterior = (rec[5] >> 16) & 0xFFFF;
    int i, j, ch;

    if (N < 2 || N > 5 || M < 2 || M > 5 || !in_ram(hptr, (u32)(N * N)) || !in_ram(cptr, (u32)(N * N * 4))) {
        return;
    }
    sGridM = M;
    for (j = 0; j < M; j++) {
        for (i = 0; i < M; i++) {
            sGrid[j][i][0] = (s32)(s8)rd8(hptr + (u32)((j * stride) * N + i * stride)) << 4;
        }
    }
    {
        // Colors: a coarser grid (LOD shift L) averaged up to M x M
        int d = L - s, cstep = d >= 0 ? (0x100 >> d) : (0x100 << -d), cmask = d >= 0 ? (1 << d) - 1 : 0;
        int Mc = ((N - 1) >> L) + 1;

        for (j = 0; j < M; j++) {
            for (i = 0; i < M; i++) {
                int idx = ((j * cstep) >> 8) * Mc + ((i * cstep) >> 8);
                int mi = (i & cmask) != 0, mj = (j & cmask) != 0;

                for (ch = 0; ch < 4; ch++) {
                    s32 a = rd8(cptr + (u32)idx * 4 + (u32)ch);
                    if (mi || mj) {
                        s32 b = rd8(cptr + (u32)(idx + (mi ? 1 : 0) + (mj ? Mc : 0)) * 4 + (u32)ch);
                        a = (a + b) >> 1;
                    }
                    sGrid[j][i][1 + ch] = a;
                }
            }
        }
    }
    if (M >= 3) {
        // Rows, then columns, then the interior (the microcode's order)
        seam(0, (int)((nbw >> 8) & 0xFF), s, rec[6] & 0xFFFF);
        seam(1, (int)(nbw & 0xFF), s, (rec[7] >> 16) & 0xFFFF);
        seam(2, (int)((nbw >> 24) & 0xFF), s, rec[5] & 0xFFFF);
        seam(3, (int)((nbw >> 16) & 0xFF), s, (rec[6] >> 16) & 0xFFFF);
        if (wInterior != 0) {
            s64 half = wInterior >> 1, inv = (0x10000u - wInterior) & 0xFFFFu;

            for (j = 1; j <= M - 2; j++) {
                for (i = 1; i <= M - 2; i++) {
                    int dj = j & 1, di = i & 1;

                    if (!di && !dj) {
                        continue;
                    }
                    for (ch = 0; ch < 5; ch++) {
                        s64 a = sGrid[j - dj][i - di][ch], b = sGrid[j + dj][i + di][ch];
                        sGrid[j][i][ch] = clamp16(((a + b) * half + (s64)sGrid[j][i][ch] * inv) >> 16);
                    }
                }
            }
        }
    }
    ensure_viewport();
    update_mvp();
    for (j = 0; j < M; j++) {
        for (i = 0; i < M; i++) {
            float col[4] = { sGrid[j][i][1] / 255.0f, sGrid[j][i][2] / 255.0f, sGrid[j][i][3] / 255.0f, sGrid[j][i][4] / 255.0f };

            tile_vertex(GRID_SLOT(i, j), (float)(x + i * size), (float)(y + sGrid[j][i][0]), (float)(z + j * size), col,
                        (float)(i * step), (float)((M - 1 - j) * step));
        }
    }
    for (j = 0; j + 1 < M; j++) {
        for (i = 0; i + 1 < M; i++) {
            gfx_tri(GRID_SLOT(i, j), GRID_SLOT(i + 1, j), GRID_SLOT(i + 1, j + 1));
            gfx_tri(GRID_SLOT(i, j), GRID_SLOT(i + 1, j + 1), GRID_SLOT(i, j + 1));
        }
    }
}

/* 05 05 02: a flat quad tile, corner heights and colors in the record */
static void tile_flat(const u32* rec) {
    s16 h[4] = { (s16)(rec[1] >> 16), (s16)rec[1], (s16)(rec[2] >> 16), (s16)rec[2] };
    u32 colw[4] = { rec[3], rec[4], rec[5], rec[6] };
    s32 x = (s16)(rec[8] >> 16), y = (s32)(s16)rec[8] << 4, z = (s16)(rec[9] >> 16), sz = (s16)rec[9];
    float s = (float)(s16)(rec[7] & 0xFFFF);
    float px[4] = { (float)x, (float)(x + sz), (float)x, (float)(x + sz) };
    float pz[4] = { (float)z, (float)z, (float)(z + sz), (float)(z + sz) };
    float us[4] = { 0, s, 0, s }, vt[4] = { s, s, 0, 0 };
    int k;

    ensure_viewport();
    update_mvp();
    for (k = 0; k < 4; k++) {
        float col[4];

        color_word(col, colw[k]);
        tile_vertex(k, px[k], (float)(y + h[k]), pz[k], col, us[k], vt[k]);
    }
    gfx_tri(0, 3, 2);
    gfx_tri(0, 1, 3);
}

/* ---------------------------------------------------------------------------
 * The display list walk
 * ------------------------------------------------------------------------- */

static void reset(void) {
    int i;

    memset(&f5, 0, sizeof(f5));
    for (i = 0; i < 4; i++) {
        f5.mv[i][i] = 1.0f;
        f5.p[i][i] = 1.0f;
    }
    f5.mvpDirty = 1;
    f5.texScale[0] = f5.texScale[1] = 0xFFFF;
}

/* A command in F3DEX encoding (geometry/other modes, texture, moveword) or
   an RDP command, through gfx.c */
static void f3dex_command(const u32* c) {
    u32 w0 = c[0], w1 = c[1];
    u8 cmd = w0 >> 24;
    Gfx g;

    if (cmd == 0xB6 || cmd == 0xB7) {
        w1 &= ~0x1000u; // texcoord perspective here, not front-face culling
    }
    if (cmd == 0xB6) {
        w1 &= ~(u32)G_ZBUFFER; // the microcode always computes depth: the other mode alone decides its use
    }
    if (!gfx_f3dex_to_f3dex2(&w0, &w1)) {
        return;
    }
    g.words.w0 = w0;
    g.words.w1 = w1;
    gfx_command(&g, w0, w1);
}

static int sStackN;
static u32 sStackBase[STACK_DEPTH], sStackPos[STACK_DEPTH];

/* The chunk after `base` (the doubly linked list's next, whose prev must be
   base), or 0 */
static u32 next_chunk(u32 base) {
    u32 next = rd32(base), target = next & 0xFFFFFF;

    if ((next >> 24) != 0x80 || target == 0 || target == base || !in_ram(target, CHUNK_SIZE) ||
        rd32(target + 4) != (0x80000000u | base)) {
        return 0;
    }
    return target;
}

static void walk_list(void* displayList) {
    u32 base, pos;
    int hops = 0, cmds = 0;

    sRam = gfx_rdram();
    if (sRam == NULL || displayList == NULL) {
        return;
    }
    if (sF5StatsOn < 0) {
        sF5StatsOn = S30_Getenv("F5STATS") != NULL;
    }
    sF5.lists++;
    if (gfx_tracing()) {
        printf("f5 list @%06X\n", (u32)((u8*)displayList - sRam));
    }
    reset();
    gfx_set_rect_side(0);
    {
        // Depth is always computed (see f3dex_command)
        u32 g[2] = { 0xB7000000u, G_ZBUFFER };
        f3dex_command(g);
    }
    sStackN = 0;
    base = (u32)((u8*)displayList - sRam);
    pos = base + 8; // the root chunk's header
    while (cmds++ < MAX_COMMANDS) {
        u32 c[10];
        u8 op;
        int extra = 0, k;

        if (pos + 8 > base + CHUNK_SIZE) {
            u32 next = next_chunk(base);
            if (next == 0 || ++hops > MAX_HOPS) {
                sF5.endLink++;
                return;
            }
            sF5.hops++;
            base = next;
            pos = base + 8;
        }
        for (k = 0; k < 10; k++) {
            c[k] = rd32(pos + k * 4);
        }
        op = c[0] >> 24;
        pos += 8;
        sF5.cmds++;
        switch (op) {
            case 0x00:
            case 0x80: // chunk header
            case 0xB0:
            case 0xB2:
            case 0xAF:
            case 0xEF:
                break;
            case 0x01:
                op_matrix(c[0], c[1] & 0xFFFFFF);
                break;
            case 0x02:
                f5.colorBuf = c[1];
                break;
            case 0x03:
                op_state(c[0], &c[2]);
                extra = 2;
                break;
            case 0x04:
                op_vertex(c[0], c[1] & 0xFFFFFF);
                sF5.verts++;
                break;
            case 0x05:
                if (((c[0] >> 16) & 0xFF) == 0x05) {
                    sF5.tiles++;
                    if ((c[0] >> 8) & 2) {
                        tile_flat(c);
                    } else {
                        tile_grid(c);
                    }
                    extra = 4;
                }
                break;
            case 0x06: // call, or a branch with bit 16
            case 0x07: { // branch
                u32 target = c[1] & 0xFFFFFF;

                if (op == 0x06 && (c[0] & 0x00FEFFFF) != 0) {
                    break; // not a call: data in the low bits
                }
                if (target == 0 || !in_ram(target, CHUNK_SIZE)) {
                    break;
                }
                sF5.calls++;
                if (op == 0x06 && !((c[0] >> 16) & 1)) {
                    if (sStackN >= STACK_DEPTH) {
                        break;
                    }
                    sStackBase[sStackN] = base;
                    sStackPos[sStackN] = pos;
                    sStackN++;
                }
                if (++hops > MAX_HOPS) {
                    return;
                }
                base = target;
                pos = base + 8;
                break;
            }
            case 0xB8: // return; at the top, the end
            case 0x0F:
                if (sStackN == 0) {
                    sF5.endTop++;
                    return;
                }
                sStackN--;
                base = sStackBase[sStackN];
                pos = sStackPos[sStackN];
                break;
            case 0xB5: // next chunk
            case 0x12: {
                u32 next = next_chunk(base);
                if (next == 0 || ++hops > MAX_HOPS) {
                    sF5.endLink++;
                    return;
                }
                sF5.hops++;
                base = next;
                pos = base + 8;
                break;
            }
            case 0xBF:
            case 0x08:
                sF5.faces++;
                extra = op_face(c, 0);
                break;
            case 0xB4:
            case 0x13:
                sF5.faces++;
                extra = op_face(c, 1);
                break;
            case 0x14:
            case 0xBE:
            case 0x09:
            case 0x0A:
                extra = 1; // 16-byte state
                break;
            case 0xBD: // billboard sprite (24 bytes)
                op_sprite(c);
                extra = 2;
                break;
            case 0xBB: // texture (a scale of 0 is 1)
            case 0x0C: {
                u32 t[2] = { (c[0] & 0x00FFFFFF) | 0xBB000000u, c[1] };
                f5.texScale[0] = (c[1] >> 16) ? (u16)(c[1] >> 16) : 0xFFFF;
                f5.texScale[1] = (c[1] & 0xFFFF) ? (u16)c[1] : 0xFFFF;
                f3dex_command(t);
                break;
            }
            case 0xBC:
            case 0x0B: {
                u32 idx = c[0] & 0xFF;
                if (idx == 8) {
                    f5.fogM = (s32)c[1] / 65536.0f;
                } else if (idx == 0x0A) {
                    f5.fogO = (s32)c[1] / 65536.0f;
                }
                break;
            }
            case 0xB6:
            case 0xB7:
            case 0xB9:
            case 0xBA:
            case 0x0D:
            case 0x0E:
            case 0x10:
            case 0x11: {
                static const u8 kAlias[] = { [0x0D] = 0xBA, [0x0E] = 0xB9, [0x10] = 0xB7, [0x11] = 0xB6 };
                u32 t[2] = { c[0], c[1] };
                if (op < 0x20) {
                    t[0] = (t[0] & 0x00FFFFFF) | ((u32)kAlias[op] << 24);
                }
                f3dex_command(t);
                break;
            }
            case 0x60: // the recomp's HUD marker: the side of the 2D element that follows
                gfx_set_rect_side((int)(c[0] & 0xFF));
                break;
            case 0xE4: // texrect, the RDP's 16-byte form
            case 0xE5:
                gfx_texrect(c[0], c[1], c[2], c[3], op == 0xE5);
                extra = 1;
                break;
            case 0xFF: { // color image: the stream also has bogus ones
                u32 fmt = (c[0] >> 21) & 7, a = c[1] & 0xFFFFFF;
                if (c[1] != 0 && fmt <= 4 && a >= 0x100000u && a < RDRAM_SIZE) {
                    sF5.cimgW = (c[0] & 0xFFF) + 1;
                    f3dex_command(c);
                }
                break;
            }
            case 0xED: // scissor: also the viewport when the stream has none
                f5.scissor[0] = c[0];
                f5.scissor[1] = c[1];
                if (gfx_tracing()) {
                    printf("f5 scissor %08X %08X @%06X\n", c[0], c[1], pos - 8);
                }
                f3dex_command(c);
                break;
            default:
                if (op >= 0xC0) {
                    f3dex_command(c);
                } else {
                    sF5.unknown++;
                }
                break;
        }
        pos += (u32)extra * 8;
    }
    sF5.endCap++;
}

void gfx_f5_walk(void* displayList) {
    walk_list(displayList);
    if (sF5StatsOn > 0 && sF5.lists % 100 == 0) {
        printf("f5: per list cmds %u faces %u vtx %u tiles %u calls %u hops %u unknown %u | ends top %u link %u cap %u (of 100) | cimg width %u\n",
               sF5.cmds / 100, sF5.faces / 100, sF5.verts / 100, sF5.tiles / 100, sF5.calls / 100, sF5.hops / 100,
               sF5.unknown / 100, sF5.endTop, sF5.endLink, sF5.endCap, sF5.cimgW);
        memset(&sF5, 0, sizeof(sF5));
        sF5.lists = 0;
    }
}
