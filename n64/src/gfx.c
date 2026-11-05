/*
 * N64 display-list renderer for the S30: interprets the F3DEX2 / RDP command
 * stream the game builds each frame and draws it with GLES2 into the
 * offscreen framebuffer (s30_display). F3DEX display lists are translated to
 * F3DEX2 command by command (f3dex_to_f3dex2). The game describes its data
 * with S30Gfx_Configure (s30_gfx.h).
 *
 * - RSP: vertices are transformed and lit on the CPU (float Mtx in this port,
 *   row-vector convention: v' = v * MV * P), triangles are batched and drawn
 *   when render state changes.
 * - RDP: TMEM is emulated as a 4 KiB byte array in N64 (big-endian) byte
 *   order; textures are decoded from it to RGBA8 and cached by content hash.
 *   The color combiner and the parts of the blender that do not read the
 *   framebuffer become a generated fragment shader; framebuffer blending maps
 *   to GL blending.
 * - Texture data in RAM is little-endian elements: u64 arrays (byte A at
 *   A ^ 7) unless the game's texElemSize says otherwise.
 * - Screen space is the N64's 320x240. 3D renders scaled into the high-res
 *   framebuffer; rectangles render 1:1 into the 2D layer (s30_display), which
 *   stores premultiplied alpha and is composited in draw order.
 */
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define F3DEX_GBI_2
#define F3DEX_GBI_PL
#include "n64/ultratypes.h"
#include "n64/gbi.h"
#include "n64/gs2dex.h"
#include "s30_display.h"
#include "s30_gfx.h"

#define SCREEN_W 320
#define SCREEN_H 240
/* Widescreen: the 480x320 panel shows N64 x through a 4:3 window centred in
   the frame, N64 x -> (x + WIDE_PAD) * FB_SCALE framebuffer pixels (WIDE_PAD
   N64 pixels of margin each side: 320 + 2 * 20 = 360 at 4:3 is 480). 3D with
   a full-screen viewport (the game world) covers the whole width with its
   clip x scaled by WIDE_XSCALE: the centre stays exactly where the 4:3
   picture had it (so the 2D layer still lines up) and the sides show more.
   Full-width fills (fades, letterbox) extend to the edges. A frame after one
   with an S2DEX background (prerendered rooms) stays 4:3: the picture can't
   widen. */
#define WIDE_PAD 20.0f
#define WIDE_XSCALE (320.0f / 360.0f)
#define FB_SCALE ((float)S30_FB_H / SCREEN_H) // both axes

#define MAX_VERTS 64
#define MAX_LIGHTS 7
#define MTX_STACK 32
#define DL_DEPTH 18

static S30GfxConfig sCfg = { S30_UCODE_F3DEX2, NULL, 1, NULL, "S30" };
#define gSegments (sCfg.segments)

void S30Gfx_Configure(const S30GfxConfig* config) {
    sCfg = *config;
    S30_SetEnvPrefix(config->envPrefix);
}

/* ---------------------------------------------------------------------------
 * Addresses
 * ------------------------------------------------------------------------- */

/* Segmented address (top byte 0x00-0x0F, through the segment table) or a
   real pointer (the binaries are linked at 0x40000000, so real pointers never
   look segmented; a game whose segments are mapped at their own addresses has
   an identity segment table). */
void* pc_resolve_addr(uint32_t addr) {
    u32 seg = addr >> 24;

    if (addr == 0) {
        return NULL;
    }
    if (seg <= 0x0F) {
        if (gSegments[seg] == 0 && seg != 0) {
            return NULL;
        }
        return (void*)(gSegments[seg] + (addr & 0x00FFFFFF));
    }
    return (void*)(uintptr_t)addr;
}

/* N64 byte order read of texture/palette data stored as little-endian u64s */
static inline u8 rd8x(const u8* p, uintptr_t xor) {
    return *(const u8*)((uintptr_t)p ^ xor);
}

/* xor for the N64 byte order of the texture data at p (element size - 1) */
static inline uintptr_t tex_xor(const void* p) {
    return sCfg.texElemSize != NULL ? (uintptr_t)sCfg.texElemSize((uintptr_t)p) - 1 : 7;
}

/* ---------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */

typedef struct {
    float x, y, z, w;
    float r, g, b, a;
    float u, v; // texture coords, 10.5 texels (after the G_TEXTURE scale)
    u8 clip; // outside x/y planes (bits 0-3); CLIP_NEAR: in front of the near plane
} RspVtx;
#define CLIP_NEAR 0x10

typedef struct {
    float col[3];
    float dir[3];      // light direction (normalized, world space)
    float mdir[3];     // in model space for the current modelview
} RspLight;

typedef struct {
    u8 fmt, siz, palette;
    u8 cms, cmt, masks, maskt, shifts, shiftt;
    u16 line, tmem;
    u16 uls, ult, lrs, lrt; // 10.2
} Tile;

static struct {
    float mv[MTX_STACK][4][4];
    int mvTop;
    float p[4][4];
    float mvp[4][4];
    int mvpDirty;
    int lightsDirty;
    u32 geom;
    RspVtx vtx[MAX_VERTS];
    RspLight lights[MAX_LIGHTS];
    float ambient[3];
    int numLights;
    float lookat[2][3];
    float mlookat[2][3];
    u16 texScaleS, texScaleT;
    int texOn;
    int texTile;
    s16 fogMul, fogOfs;
    u32 rdpHalf1, rdpHalf2;
    // viewport in N64 screen pixels
    float vpX, vpY, vpW, vpH;
} rsp;

static struct {
    u32 otherH, otherL;
    u32 combW0, combW1;
    float prim[4], env[4], fog[4], blend[4], fill[4];
    float primLod;
    u16 primDepth;
    Tile tiles[8];
    u8 timgFmt, timgSiz;
    u16 timgWidth;
    u32 timgAddr;
    u32 cimg, zimg;
    int cimgIsFrame; // the color image is the 320-wide frame
    int scX, scY, scW, scH; // scissor (N64 pixels)
} rdp;

/* Debug counters, printed every 100 frames when <PREFIX>_GFXSTATS is set */
static struct {
    u32 cmds, tris, trisSkipped, trisClipped, draws, rects, fills, cimgs, dls, vtx, uploads, switches;
} sStats;
static int sStatsOn = -1;
static u32 sTraceFrame; // <PREFIX>_GFXTRACE=frame: log every draw of that frame
static volatile sig_atomic_t sTraceRequest; // SIGUSR2: trace the next frame

static void on_sigusr2(int sig) {
    (void)sig;
    sTraceRequest = 1;
}

static void flush(void);
// sWide: this frame is widescreen (declared above frame_copies)
static int sHadBg; // an S2DEX background was drawn this frame
static void aux_sync(const u8* src, u32 bytes);

static u8 sTmem[4096];
static u16 sTlut[256];
/* Combiner bits of the first cycle (a0 c0 Aa0 Ac0 | b0 d0 Ab0 Ad0) */
#define COMB1_W0 0x00FFFE00u
#define COMB1_W1 0xF003FE00u

static u32 sTmemGen = 1;
static int sBatchTile; // render tile of the pending batch (texture coordinates, binding)
/* Why batches end (<PREFIX>_GFXSTATS): the draw count is what costs on lima */
enum {
    FR_AUXSYNC,
    FR_BG,
    FR_BLENDCOL,
    FR_CIMG,
    FR_COMBINE,
    FR_FILLCOL,
    FR_FOGCOL,
    FR_FRAME,
    FR_GEOM,
    FR_LOADBLOCK,
    FR_LOADTILE,
    FR_MARK,
    FR_OMODEH,
    FR_OMODEL,
    FR_PRIMLOD,
    FR_RDPOMODE,
    FR_SCISSOR,
    FR_TEXTURE,
    FR_TILE,
    FR_TLUT,
    FR_VIEWPORT,
    FR_ZCLEAR,
    FR_ZIMG,
    FR_BATCH_SPACE,
    FR_BATCH_TILE,
    FR_BATCH_FULL,
    FR_COUNT
};
static const char* const kFlushNames[FR_COUNT] = {"auxsync", "bg", "blendcol", "cimg", "combine", "fillcol", "fogcol", "frame", "geom", "loadblock", "loadtile", "mark", "omodeH", "omodeL", "primlod", "rdpomode", "scissor", "texture", "tile", "tlut", "viewport", "zclear", "zimg", "batch-space", "batch-tile", "batch-full"};
static u32 sFlushWhy[FR_COUNT];
static int sFlushReason = -1;
#define flush_for(r) (sFlushReason = (r), flush(), sFlushReason = -1)

/* ---------------------------------------------------------------------------
 * Math
 * ------------------------------------------------------------------------- */

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

static void normalize3(float v[3]) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

    if (l > 0.0f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

/* World-space direction into the current model space: d * transpose(MV) */
static void to_model_dir(float out[3], const float d[3]) {
    float(*m)[4] = rsp.mv[rsp.mvTop];

    out[0] = d[0] * m[0][0] + d[1] * m[0][1] + d[2] * m[0][2];
    out[1] = d[0] * m[1][0] + d[1] * m[1][1] + d[2] * m[1][2];
    out[2] = d[0] * m[2][0] + d[1] * m[2][1] + d[2] * m[2][2];
    normalize3(out);
}

static void update_lights(void) {
    int i;

    for (i = 0; i < rsp.numLights; i++) {
        to_model_dir(rsp.lights[i].mdir, rsp.lights[i].dir);
    }
    to_model_dir(rsp.mlookat[0], rsp.lookat[0]);
    to_model_dir(rsp.mlookat[1], rsp.lookat[1]);
    rsp.lightsDirty = 0;
}

/* <PREFIX>_PROFILE: a sampling profiler. SIGPROF every 1 ms of process CPU time
   records the interrupted pc; every 500 frames the counts and the memory map
   go to /tmp/s30_prof.txt (symbolize the binary's with addr2line, libraries
   by their map). */
#include <sys/time.h>
#include <ucontext.h>

#define PROF_MAX (1 << 20)
static u32* sProfPc;
static volatile u32 sProfN;

static void prof_sample(int sig, siginfo_t* info, void* ctx) {
    u32 n = sProfN;

    (void)sig;
    (void)info;
    if (n < PROF_MAX) {
        sProfPc[n] = (u32)((ucontext_t*)ctx)->uc_mcontext.arm_pc;
        sProfN = n + 1;
    }
}

static int prof_cmp(const void* a, const void* b) {
    u32 x = *(const u32*)a, y = *(const u32*)b;
    return x < y ? -1 : x > y;
}

static void prof_tick(u32 frame) {
    static int on = -1;
    FILE *f, *maps;
    u32 n, i, run;
    u32* copy;
    char line[512];

    if (on < 0) {
        struct sigaction sa;
        struct itimerval it = { { 0, 1000 }, { 0, 1000 } };

        on = S30_Getenv("PROFILE") != NULL;
        if (!on) {
            return;
        }
        sProfPc = malloc(PROF_MAX * sizeof(u32));
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = prof_sample;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGPROF, &sa, NULL);
        setitimer(ITIMER_PROF, &it, NULL);
        printf("gfx: profiling to /tmp/s30_prof.txt\n");
    }
    if (!on || frame % 500 != 0 || (n = sProfN) == 0) {
        return;
    }
    copy = malloc(n * sizeof(u32));
    memcpy(copy, sProfPc, n * sizeof(u32));
    qsort(copy, n, sizeof(u32), prof_cmp);
    f = fopen("/tmp/s30_prof.txt.tmp", "w");
    if (f != NULL) {
        fprintf(f, "samples %u frames %u\n", n, frame);
        maps = fopen("/proc/self/maps", "r");
        while (maps != NULL && fgets(line, sizeof(line), maps) != NULL) {
            if (strstr(line, " r-xp ") != NULL) {
                fprintf(f, "map %s", line);
            }
        }
        if (maps != NULL) {
            fclose(maps);
        }
        for (i = 0; i < n; i += run) {
            for (run = 1; i + run < n && copy[i + run] == copy[i]; run++) {
            }
            fprintf(f, "%08x %u\n", (unsigned)copy[i], (unsigned)run);
        }
        fclose(f);
        rename("/tmp/s30_prof.txt.tmp", "/tmp/s30_prof.txt");
    }
    free(copy);
}

static double now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static double sTFlush, sTTex, sTDraw; // profiling (<PREFIX>_GFXSTATS)
/* Per-draw timers: clock_gettime is a system call here (no vDSO clock), so
   only while stats are on */
static inline double stat_ms(void) {
    return sStatsOn > 1 ? now_ms() : 0.0; // <PREFIX>_GFXSTATS=2
}

/* ---------------------------------------------------------------------------
 * GL state cache: lima is expensive per call, so only send changes. Anything
 * outside this file that touches GL state (the 2D composite, the display
 * blit) is followed by gl_cache_reset(), which poisons every cached field
 * (all bits set never matches a real value) so each one is re-sent on its
 * next use.
 * ------------------------------------------------------------------------- */

static struct {
    GLuint prog;
    int blend, depthTest, depthMask, cull, polyOfs, scissor;
    GLenum bsrc, bdst, basrc, badst, depthFunc, cullFace;
    GLint vp[4], sc[4];
    GLuint tex[2];
} sGL;

static int sAttribsSet;

static void gl_cache_reset(void) {
    memset(&sGL, 0xFF, sizeof(sGL));
    sAttribsSet = 0;
}

static void gl_enable(GLenum cap, int on, int* cur) {
    if (*cur != on) {
        if (on) {
            glEnable(cap);
        } else {
            glDisable(cap);
        }
        *cur = on;
    }
}

static void gl_blend_func(GLenum s, GLenum d, GLenum as, GLenum ad) {
    if (sGL.bsrc != s || sGL.bdst != d || sGL.basrc != as || sGL.badst != ad) {
        glBlendFuncSeparate(s, d, as, ad);
        sGL.bsrc = s;
        sGL.bdst = d;
        sGL.basrc = as;
        sGL.badst = ad;
    }
}

static void gl_viewport(GLint x, GLint y, GLint w, GLint h) {
    if (sGL.vp[0] != x || sGL.vp[1] != y || sGL.vp[2] != w || sGL.vp[3] != h) {
        glViewport(x, y, w, h);
        sGL.vp[0] = x;
        sGL.vp[1] = y;
        sGL.vp[2] = w;
        sGL.vp[3] = h;
    }
}

static void gl_scissor(GLint x, GLint y, GLint w, GLint h) {
    if (sGL.sc[0] != x || sGL.sc[1] != y || sGL.sc[2] != w || sGL.sc[3] != h) {
        glScissor(x, y, w, h);
        sGL.sc[0] = x;
        sGL.sc[1] = y;
        sGL.sc[2] = w;
        sGL.sc[3] = h;
    }
}

/* ---------------------------------------------------------------------------
 * Shaders (combiner + blender), generated per mode and cached
 * ------------------------------------------------------------------------- */

enum { IN_ZERO, IN_ONE, IN_COMBINED, IN_TEX0, IN_TEX1, IN_PRIM, IN_SHADE, IN_ENV, IN_NOISE,
       IN_COMBINED_A, IN_TEX0_A, IN_TEX1_A, IN_PRIM_A, IN_SHADE_A, IN_ENV_A, IN_PRIM_LOD };

static int cc_a(int v) {
    static const s8 t[8] = { IN_COMBINED, IN_TEX0, IN_TEX1, IN_PRIM, IN_SHADE, IN_ENV, IN_ONE, IN_NOISE };
    return v < 8 ? t[v] : IN_ZERO;
}
static int cc_b(int v) {
    static const s8 t[8] = { IN_COMBINED, IN_TEX0, IN_TEX1, IN_PRIM, IN_SHADE, IN_ENV, IN_ZERO, IN_ZERO };
    return v < 8 ? t[v] : IN_ZERO;
}
static int cc_c(int v) {
    static const s8 t[16] = { IN_COMBINED, IN_TEX0, IN_TEX1, IN_PRIM, IN_SHADE, IN_ENV, IN_ZERO, IN_COMBINED_A,
                              IN_TEX0_A, IN_TEX1_A, IN_PRIM_A, IN_SHADE_A, IN_ENV_A, IN_ZERO, IN_PRIM_LOD, IN_ZERO };
    return v < 16 ? t[v] : IN_ZERO;
}
static int cc_d(int v) {
    static const s8 t[8] = { IN_COMBINED, IN_TEX0, IN_TEX1, IN_PRIM, IN_SHADE, IN_ENV, IN_ONE, IN_ZERO };
    return t[v & 7];
}
static int ac_abd(int v) {
    static const s8 t[8] = { IN_COMBINED, IN_TEX0, IN_TEX1, IN_PRIM, IN_SHADE, IN_ENV, IN_ONE, IN_ZERO };
    return t[v & 7];
}
static int ac_c(int v) {
    static const s8 t[8] = { IN_ZERO, IN_TEX0, IN_TEX1, IN_PRIM, IN_SHADE, IN_ENV, IN_PRIM_LOD, IN_ZERO };
    return t[v & 7];
}

static const char* color_src(int in) {
    switch (in) {
        case IN_ONE: return "vec3(1.0)";
        case IN_COMBINED: return "cc";
        case IN_TEX0: return "t0.rgb";
        case IN_TEX1: return "t1.rgb";
        case IN_PRIM: return "vPrim.rgb";
        case IN_SHADE: return "vShade.rgb";
        case IN_ENV: return "vEnv.rgb";
        // The RDP's noise, new every frame: a random texture (texture unit 2,
        // S30Gfx_Init) at the pixel, offset per frame. A sin() hash needs more
        // than the Mali-400's 16-bit fragment floats.
        case IN_NOISE: return "vec3(texture2D(uNoiseTex, gl_FragCoord.xy / 64.0 + uNoise).r)";
        case IN_COMBINED_A: return "vec3(ca)";
        case IN_TEX0_A: return "vec3(t0.a)";
        case IN_TEX1_A: return "vec3(t1.a)";
        case IN_PRIM_A: return "vec3(vPrim.a)";
        case IN_SHADE_A: return "vec3(vShade.a)";
        case IN_ENV_A: return "vec3(vEnv.a)";
        case IN_PRIM_LOD: return "vec3(uPrimLod)";
        default: return "vec3(0.0)";
    }
}

static const char* alpha_src(int in) {
    switch (in) {
        case IN_ONE: return "1.0";
        case IN_COMBINED: return "ca";
        case IN_TEX0: return "t0.a";
        case IN_TEX1: return "t1.a";
        case IN_PRIM: return "vPrim.a";
        case IN_SHADE: return "vShade.a";
        case IN_ENV: return "vEnv.a";
        case IN_PRIM_LOD: return "uPrimLod";
        default: return "0.0";
    }
}

/* Shader key: combiner inputs + flags */
enum {
    SF_2CYC = 1 << 0,
    SF_FOG = 1 << 1,          // blender cycle 1 mixes in the fog color by shade alpha
    SF_ALPHA_THRESH = 1 << 2, // alpha compare against blend color alpha
    SF_TEX_EDGE = 1 << 3,     // coverage from alpha (cutout)
    SF_FILL = 1 << 4,         // fill mode: fill color only
    SF_COPY = 1 << 5,         // copy mode: texel 0 only
    SF_DBG_ALPHA = 1 << 6,    // debug: output alpha as grey
    SF_3POINT = 1 << 7,       // N64 3-point bilinear filter (textures are GL_NEAREST)
    SF_CVG_ALPHA = 1 << 8,    // antialiased coverage times alpha: no coverage, no write (depth either)
};

typedef struct {
    u32 w0, w1, flags;
    GLuint prog;
    GLint uPrim, uEnv, uFog, uBlend, uFill, uPrimLod, uTexSize0, uTexSize1, uNoise;
    float cur[5][4]; // last uploaded prim, env, fog, blend, fill
    float curLod;
    u32 curNoise; // the frame its noise offset is for
    float curSize[2][2];
    int usesTex[2];
} Shader;

#define MAX_SHADERS 256
static Shader sShaders[MAX_SHADERS];
static int sNumShaders;
static Shader* sCurShader;

enum { ATTR_POS, ATTR_COLOR, ATTR_UV0, ATTR_UV1, ATTR_FOG, ATTR_PRIM, ATTR_ENV };

static void emit_cycle(char* s, size_t n, const int c[4], const int a[4], int last) {
    size_t len = strlen(s);

    // Clamp color and alpha as one vec4: lima miscompiles separate clamps on
    // the rgb and alpha halves (a texture alpha feeding only ca reads as 0)
    snprintf(s + len, n - len,
             "  cm = clamp(vec4((%s - %s) * %s + %s, (%s - %s) * %s + %s), 0.0, 1.0);\n"
             "  cc = cm.rgb; ca = cm.a;\n",
             color_src(c[0]), color_src(c[1]), color_src(c[2]), color_src(c[3]), alpha_src(a[0]),
             alpha_src(a[1]), alpha_src(a[2]), alpha_src(a[3]));
    (void)last;
}

static int uses_input(const int c[2][4], const int a[2][4], int ncyc, int in, int inA) {
    int i, k;

    for (i = 0; i < ncyc; i++) {
        for (k = 0; k < 4; k++) {
            if (c[i][k] == in || c[i][k] == inA || a[i][k] == in) {
                return 1;
            }
        }
    }
    return 0;
}

static Shader* get_shader(u32 w0, u32 w1, u32 flags) {
    static const char* vs =
        "attribute vec4 aPos; attribute vec4 aColor; attribute vec2 aUv0; attribute vec2 aUv1;\n"
        "attribute float aFog; attribute vec4 aPrim; attribute vec4 aEnv;\n"
        "varying vec4 vShade; varying vec2 vUv0; varying vec2 vUv1; varying float vFog;\n"
        "varying vec4 vPrim; varying vec4 vEnv;\n"
        "void main() { vShade = aColor; vUv0 = aUv0; vUv1 = aUv1; vFog = aFog; vPrim = aPrim; vEnv = aEnv;\n"
        "  gl_Position = aPos; }\n";
    char fs[4096];
    int c[2][4], a[2][4];
    int i, ncyc;
    Shader* sh;

    for (i = 0; i < sNumShaders; i++) {
        sh = &sShaders[i];
        if (sh->w0 == w0 && sh->w1 == w1 && sh->flags == flags) {
            return sh;
        }
    }
    if (sNumShaders == MAX_SHADERS) {
        // Drop the cache; shaders are cheap to rebuild relative to running out
        for (i = 0; i < sNumShaders; i++) {
            glDeleteProgram(sShaders[i].prog);
        }
        sNumShaders = 0;
    }

    c[0][0] = cc_a((w0 >> 20) & 0xF);
    c[0][1] = cc_b((w1 >> 28) & 0xF);
    c[0][2] = cc_c((w0 >> 15) & 0x1F);
    c[0][3] = cc_d((w1 >> 15) & 0x7);
    a[0][0] = ac_abd((w0 >> 12) & 0x7);
    a[0][1] = ac_abd((w1 >> 12) & 0x7);
    a[0][2] = ac_c((w0 >> 9) & 0x7);
    a[0][3] = ac_abd((w1 >> 9) & 0x7);
    c[1][0] = cc_a((w0 >> 5) & 0xF);
    c[1][1] = cc_b((w1 >> 24) & 0xF);
    c[1][2] = cc_c(w0 & 0x1F);
    c[1][3] = cc_d((w1 >> 6) & 0x7);
    a[1][0] = ac_abd((w1 >> 21) & 0x7);
    a[1][1] = ac_abd((w1 >> 3) & 0x7);
    a[1][2] = ac_c((w1 >> 18) & 0x7);
    a[1][3] = ac_abd(w1 & 0x7);
    ncyc = (flags & SF_2CYC) ? 2 : 1;

    sh = &sShaders[sNumShaders++];
    memset(sh, 0, sizeof(*sh));
    sh->w0 = w0;
    sh->w1 = w1;
    sh->flags = flags;

    snprintf(fs, sizeof(fs),
             "precision mediump float;\n"
             "varying vec4 vShade; varying vec2 vUv0; varying vec2 vUv1; varying float vFog;\n"
             "varying vec4 vPrim; varying vec4 vEnv;\n"
             "uniform sampler2D uTex0; uniform sampler2D uTex1;\n"
             "uniform vec4 uFog; uniform vec4 uBlend;\n"
             "uniform vec4 uFill; uniform float uPrimLod; uniform vec2 uNoise; uniform sampler2D uNoiseTex;\n"
             "uniform vec2 uTexSize0; uniform vec2 uTexSize1;\n"
             /* N64 RDP bilinear: blend 3 texels (the triangle of the 2x2 quad
                the sample falls in), from nearest-filtered fetches */
             "vec4 tex3(sampler2D t, vec2 uv, vec2 size) {\n"
             "  vec2 f = fract(uv * size - 0.5);\n"
             "  f -= step(1.0, f.x + f.y);\n"
             "  vec4 c0 = texture2D(t, uv - f / size);\n"
             "  vec4 c1 = texture2D(t, uv - vec2(f.x - sign(f.x), f.y) / size);\n"
             "  vec4 c2 = texture2D(t, uv - vec2(f.x, f.y - sign(f.y)) / size);\n"
             "  return c0 + abs(f.x) * (c1 - c0) + abs(f.y) * (c2 - c0);\n"
             "}\n"
             "void main() {\n"
             "  vec4 t0 = vec4(0.0); vec4 t1 = vec4(0.0);\n"
             "  vec3 cc = vec3(0.0); float ca = 0.0; vec4 cm;\n");
    if (flags & SF_FILL) {
        strcat(fs, "  cc = uFill.rgb; ca = uFill.a;\n");
    } else if (flags & SF_COPY) {
        sh->usesTex[0] = 1;
        strcat(fs, "  t0 = texture2D(uTex0, vUv0); cc = t0.rgb; ca = t0.a;\n");
    } else {
        sh->usesTex[0] = uses_input(c, a, ncyc, IN_TEX0, IN_TEX0_A);
        sh->usesTex[1] = uses_input(c, a, ncyc, IN_TEX1, IN_TEX1_A);
        if (sh->usesTex[0]) {
            strcat(fs, (flags & SF_3POINT) ? "  t0 = tex3(uTex0, vUv0, uTexSize0);\n"
                                           : "  t0 = texture2D(uTex0, vUv0);\n");
        }
        if (sh->usesTex[1]) {
            strcat(fs, (flags & SF_3POINT) ? "  t1 = tex3(uTex1, vUv1, uTexSize1);\n"
                                           : "  t1 = texture2D(uTex1, vUv1);\n");
        }
        for (i = 0; i < ncyc; i++) {
            emit_cycle(fs, sizeof(fs), c[i], a[i], i == ncyc - 1);
        }
    }
    if (flags & SF_TEX_EDGE) {
        strcat(fs, "  if (ca < 0.5) discard;\n");
    }
    if (flags & SF_CVG_ALPHA) {
        // Coverage (8 levels) times alpha rounds to none below 1/8
        strcat(fs, "  if (ca < 0.125) discard;\n");
    }
    if (flags & SF_ALPHA_THRESH) {
        // uBlend.a arrives clamped to >= 1/255: lima miscompiles a max()
        // in this compare and discards every fragment
        strcat(fs, "  if (ca < uBlend.a) discard;\n");
    }
    if (flags & SF_FOG) {
        strcat(fs, "  cc = mix(cc, uFog.rgb, vFog);\n");
    }
    strcat(fs, (flags & SF_DBG_ALPHA) ? "  gl_FragColor = vec4(vec3(ca), 1.0);\n}\n"
                                       : "  gl_FragColor = vec4(cc, ca);\n}\n");
    if (S30_Getenv("SHADERDUMP")) {
        printf("gfx: shader %06x:%08x flags %x\n%s", w0, w1, flags, fs);
    }

    {
        GLuint p = glCreateProgram();
        GLuint v = glCreateShader(GL_VERTEX_SHADER);
        GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
        const char* src;
        GLint ok;

        src = vs;
        glShaderSource(v, 1, &src, NULL);
        glCompileShader(v);
        src = fs;
        glShaderSource(f, 1, &src, NULL);
        glCompileShader(f);
        glGetShaderiv(f, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetShaderInfoLog(f, sizeof(log), NULL, log);
            printf("gfx: shader compile failed: %s\n%s\n", log, fs);
        }
        glAttachShader(p, v);
        glAttachShader(p, f);
        glBindAttribLocation(p, ATTR_POS, "aPos");
        glBindAttribLocation(p, ATTR_COLOR, "aColor");
        glBindAttribLocation(p, ATTR_UV0, "aUv0");
        glBindAttribLocation(p, ATTR_UV1, "aUv1");
        glBindAttribLocation(p, ATTR_FOG, "aFog");
        glBindAttribLocation(p, ATTR_PRIM, "aPrim");
        glBindAttribLocation(p, ATTR_ENV, "aEnv");
        glLinkProgram(p);
        glDeleteShader(v);
        glDeleteShader(f);
        sh->prog = p;
        glUseProgram(p);
        gl_cache_reset();
        memset(sh->cur, 0xFF, sizeof(sh->cur)); // NaNs: first upload always happens
        memset(&sh->curLod, 0xFF, sizeof(sh->curLod));
        memset(sh->curSize, 0xFF, sizeof(sh->curSize));
        glUniform1i(glGetUniformLocation(p, "uTex0"), 0);
        glUniform1i(glGetUniformLocation(p, "uTex1"), 1);
        glUniform1i(glGetUniformLocation(p, "uNoiseTex"), 2);
        sh->uPrim = glGetUniformLocation(p, "uPrim");
        sh->uEnv = glGetUniformLocation(p, "uEnv");
        sh->uFog = glGetUniformLocation(p, "uFog");
        sh->uBlend = glGetUniformLocation(p, "uBlend");
        sh->uFill = glGetUniformLocation(p, "uFill");
        sh->uPrimLod = glGetUniformLocation(p, "uPrimLod");
        sh->uNoise = glGetUniformLocation(p, "uNoise");
        sh->curNoise = 0xFFFFFFFF;
        sh->uTexSize0 = glGetUniformLocation(p, "uTexSize0");
        sh->uTexSize1 = glGetUniformLocation(p, "uTexSize1");
    }
    return sh;
}

/* ---------------------------------------------------------------------------
 * Textures: TMEM decode + content-hashed GL texture cache
 * ------------------------------------------------------------------------- */

typedef struct {
    u64 hash;
    GLuint tex;
    u16 w, h;
    u32 lastUse;
    GLint filter, wrapS, wrapT; // current GL parameters (0 = unset)
    u8 mipmapped;               // a replacement texture's mipmaps: minify with them
} TexEntry;

/* Replacement textures (S30Gfx_TextureOverride): a tile loaded into TMEM from
   `src` draws `pixels` instead, at their own resolution (texture coordinates
   stay the tile's, normalised). The GL texture is made at first use. */
typedef struct {
    const void* src;
    void* pixels; // freed once uploaded
    int w, h, rgb565;
    // A copy of the last frame instead of pixels (S30Gfx_FrameCopy): the
    // N64 screen rectangle, copied at the start of the next frame
    int frame, frameX, frameY, framePending;
    GLuint frameFbo;
    TexEntry entry;
} TexOverride;

static u32 sFrame; // (defined below)

#define TEX_OVERRIDE_MAX 48
static TexOverride sTexOverrides[TEX_OVERRIDE_MAX];
static int sTexOverrideCount;
static const u8* sTmemSrc[512]; // where each TMEM line (8 bytes) was last loaded from, by its first line

void S30Gfx_TextureOverride(const void* src, int w, int h, int rgb565, void* pixels) {
    int i;

    for (i = 0; i < sTexOverrideCount; i++) {
        if (sTexOverrides[i].src == src) {
            break;
        }
    }
    if (i == TEX_OVERRIDE_MAX) {
        free(pixels); // full
        return;
    }
    if ((i < sTexOverrideCount) && (sTexOverrides[i].pixels == pixels)) {
        return; // registered already
    }
    sTexOverrides[i].pixels = pixels;
    sTexOverrides[i].w = w;
    sTexOverrides[i].h = h;
    sTexOverrides[i].rgb565 = rgb565;
    sTexOverrides[i].src = src;
    __sync_synchronize(); // the entry before the count that publishes it (the renderer may be another thread)
    if (i == sTexOverrideCount) {
        sTexOverrideCount++;
    }
}

/* S30Gfx_ReadFrame: the frame so far into the game's RAM as 320x240 RGBA16
   (S30_MARK_FB_READ), each N64 pixel from the framebuffer pixel at its
   centre */
static u16* sReadFrameDst;

void S30Gfx_ReadFrame(void* dst) {
    sReadFrameDst = dst;
}

static void read_frame(void) {
    static u8 px[S30_FB_W * S30_FB_H * 4];
    u16* dst = sReadFrameDst;
    int x, y;

    if (dst == NULL) {
        return;
    }
    sReadFrameDst = NULL;
    S30Display_ReadFrame(px);
    for (y = 0; y < SCREEN_H; y++) {
        int fy = (int)((SCREEN_H - y - 0.5f) * FB_SCALE);

        for (x = 0; x < SCREEN_W; x++) {
            int fx = (int)((x + WIDE_PAD + 0.5f) * FB_SCALE);
            const u8* p = &px[(fy * S30_FB_W + fx) * 4];

            dst[y * SCREEN_W + x] = ((p[0] >> 3) << 11) | ((p[1] >> 3) << 6) | ((p[2] >> 3) << 1) | 1;
        }
    }
}

void S30Gfx_FrameCopy(const void* dst, int x, int y, int w, int h) {
    TexOverride* o;
    int i;

    for (i = 0; i < sTexOverrideCount; i++) {
        if (sTexOverrides[i].src == dst) {
            break;
        }
    }
    if (i == TEX_OVERRIDE_MAX) {
        return; // full
    }
    o = &sTexOverrides[i];
    o->frame = 1;
    o->frameX = x;
    o->frameY = y;
    o->w = w;
    o->h = h;
    o->framePending = 1;
    o->src = dst;
    if (i == sTexOverrideCount) {
        sTexOverrideCount++;
    }
}

static int sWide;

/* Pending S30Gfx_FrameCopy: the last frame's rectangles into their textures
   (before this frame draws over it). Widescreen frames show more of the world
   at the sides: the N64 rectangle is where that picture is now. */
static void frame_copies(void) {
    int i, copied = 0;

    for (i = 0; i < sTexOverrideCount; i++) {
        TexOverride* o = &sTexOverrides[i];
        float x0 = o->frameX, x1 = o->frameX + o->w;

        if (!o->frame || !o->framePending) {
            continue;
        }
        o->framePending = 0;
        if (sWide) {
            x0 = SCREEN_W / 2 + (x0 - SCREEN_W / 2) * WIDE_XSCALE;
            x1 = SCREEN_W / 2 + (x1 - SCREEN_W / 2) * WIDE_XSCALE;
        }
        S30Display_CopyRect(&o->entry.tex, &o->frameFbo, o->w, o->h, (x0 + WIDE_PAD) * FB_SCALE,
                            o->frameY * FB_SCALE, (x1 + WIDE_PAD) * FB_SCALE, (o->frameY + o->h) * FB_SCALE);
        o->entry.mipmapped = 0;
        copied = 1;
    }
    if (copied) {
        S30Display_Unbind();
        gl_cache_reset();
    }
}

/* The replacement for a tile, if its TMEM was loaded from a replaced texture */
static TexEntry* tex_override(const Tile* t, int w, int h) {
    const u8* src = sTmemSrc[t->tmem & 511];
    int i;

    for (i = 0; i < sTexOverrideCount; i++) {
        TexOverride* o = &sTexOverrides[i];

        if (o->src != src) {
            continue;
        }
        if (o->frame) {
            if (o->entry.tex == 0) {
                return NULL; // not copied yet
            }
        } else if (o->entry.tex == 0) {
            glGenTextures(1, &o->entry.tex);
            glBindTexture(GL_TEXTURE_2D, o->entry.tex);
            if (o->rgb565) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, o->w, o->h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, o->pixels);
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, o->w, o->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, o->pixels);
            }
            glGenerateMipmap(GL_TEXTURE_2D); // sizes are powers of 2 (GLES2)
            o->entry.mipmapped = 1;
            free(o->pixels); // the GL texture has its own copy
            o->pixels = NULL;
            sGL.tex[0] = sGL.tex[1] = 0; // bound above: rebind as needed
        }
        o->entry.w = w;
        o->entry.h = h;
        o->entry.lastUse = sFrame;
        return &o->entry;
    }
    return NULL;
}

#define TEX_CACHE_SIZE 2048
static TexEntry sTexCache[TEX_CACHE_SIZE];
static u32 sFrame;
static int sHasNpot;

typedef struct {
    u32 gen;
    Tile tile;
    u32 tlutMode;
    TexEntry* entry;
    u64 hash; // entry->hash when cached: the entry may since hold another texture
} TileCache;
static TileCache sTileCache[8];

static u8 sDecodeBuf[512 * 512 * 4];

/* Content hash for the texture cache: two 32-bit FNV-1a lanes over
   alternating words (a 64-bit multiply per word is several instructions on
   32-bit ARM; this hashing was ~1.5% of the frame) */
static u64 hash_bytes(const u8* p, size_t n, u64 h) {
    u32 a = (u32)h, b = (u32)(h >> 32);
    size_t i = 0;

    for (; i + 8 <= n; i += 8) {
        u32 w0, w1;
        memcpy(&w0, p + i, 4);
        memcpy(&w1, p + i + 4, 4);
        a = (a ^ w0) * 0x01000193u;
        b = (b ^ w1) * 0x01000193u;
    }
    for (; i < n; i++) {
        a = (a ^ p[i]) * 0x01000193u;
        b = (b + p[i]) * 0x01000193u;
    }
    return ((u64)b << 32 | a) ^ ((u64)a << 17);
}

static inline void put_rgba(u8* d, u8 r, u8 g, u8 b, u8 a) {
    d[0] = r;
    d[1] = g;
    d[2] = b;
    d[3] = a;
}

static void color16(u8* d, u16 c, u32 tlutMode) {
    if (tlutMode == G_TT_IA16) {
        put_rgba(d, c >> 8, c >> 8, c >> 8, c & 0xFF);
    } else {
        u8 r = (c >> 11) & 31, g = (c >> 6) & 31, b = (c >> 1) & 31;
        put_rgba(d, (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), (c & 1) ? 255 : 0);
    }
}

/* Texel (x, y) of a tile from TMEM, decoded to RGBA8 */
static void decode_texel(u8* d, const Tile* t, u32 pitch, int x, int y, u32 tlutMode) {
    u32 base = t->tmem * 8 + y * pitch;
    u8 b, n;

    switch (t->siz) {
        case G_IM_SIZ_4b:
            b = sTmem[(base + (x >> 1)) & 0xFFF];
            n = (x & 1) ? (b & 0xF) : (b >> 4);
            switch (t->fmt) {
                case G_IM_FMT_CI:
                    color16(d, sTlut[(t->palette << 4) | n], tlutMode);
                    break;
                case G_IM_FMT_IA: {
                    u8 i = ((n >> 1) * 255) / 7;
                    put_rgba(d, i, i, i, (n & 1) ? 255 : 0);
                    break;
                }
                default: // I4
                    put_rgba(d, n * 17, n * 17, n * 17, n * 17);
                    break;
            }
            break;
        case G_IM_SIZ_8b:
            b = sTmem[(base + x) & 0xFFF];
            switch (t->fmt) {
                case G_IM_FMT_CI:
                    color16(d, sTlut[b], tlutMode);
                    break;
                case G_IM_FMT_IA:
                    put_rgba(d, (b >> 4) * 17, (b >> 4) * 17, (b >> 4) * 17, (b & 0xF) * 17);
                    break;
                default: // I8
                    put_rgba(d, b, b, b, b);
                    break;
            }
            break;
        case G_IM_SIZ_16b: {
            u32 o = base + x * 2;
            u16 c = (sTmem[o & 0xFFF] << 8) | sTmem[(o + 1) & 0xFFF];
            if (t->fmt == G_IM_FMT_IA) {
                put_rgba(d, c >> 8, c >> 8, c >> 8, c & 0xFF);
            } else if (t->fmt == G_IM_FMT_CI) {
                color16(d, sTlut[c & 0xFF], tlutMode);
            } else {
                color16(d, c, 0);
            }
            break;
        }
        default: { // 32b RGBA, stored linearly (see load_block / load_tile)
            u32 o = base + x * 4;
            put_rgba(d, sTmem[o & 0xFFF], sTmem[(o + 1) & 0xFFF], sTmem[(o + 2) & 0xFFF], sTmem[(o + 3) & 0xFFF]);
            break;
        }
    }
}

static u32 tile_pitch(const Tile* t) {
    return t->siz == G_IM_SIZ_32b ? t->line * 16 : t->line * 8;
}

/* Decoded texture size. A clamped axis covers the whole tile (mask/mirror
   applied while decoding, see tile_texel); a wrapping axis is one mask period
   and GL repeats/mirrors it. */
static void tile_dims(const Tile* t, int* w, int* h) {
    int tw = ((t->lrs - t->uls) >> 2) + 1;
    int th = ((t->lrt - t->ult) >> 2) + 1;

    if (t->masks && !(t->cms & G_TX_CLAMP)) {
        tw = 1 << t->masks;
    }
    if (t->maskt && !(t->cmt & G_TX_CLAMP)) {
        th = 1 << t->maskt;
    }
    if (tw < 1 || tw > 1024) {
        tw = 1;
    }
    if (th < 1 || th > 1024) {
        th = 1;
    }
    *w = tw;
    *h = th;
}

/* Texel coordinate inside TMEM for a decoded coordinate (N64 mask + mirror) */
static int tile_texel(int x, int mask, int cm) {
    if (mask) {
        int m = 1 << mask;
        int r = x & (m - 1);
        return ((cm & G_TX_MIRROR) && (x & m)) ? m - 1 - r : r;
    }
    return x;
}

static int is_pot(int v) {
    return (v & (v - 1)) == 0;
}

static GLint wrap_mode(int cm, int mask, int dim) {
    // No mask: the N64 does not wrap that axis
    if ((cm & G_TX_CLAMP) || mask == 0 || (!sHasNpot && !is_pot(dim))) {
        return GL_CLAMP_TO_EDGE;
    }
    return (cm & G_TX_MIRROR) ? GL_MIRRORED_REPEAT : GL_REPEAT;
}

static TexEntry* tex_lookup(int tileIdx, int* outW, int* outH) {
    Tile* t = &rdp.tiles[tileIdx & 7];
    TileCache* tc = &sTileCache[tileIdx & 7];
    u32 tlutMode = rdp.otherH & (3 << G_MDSFT_TEXTLUT);
    int w, h, x, y;
    u32 pitch = tile_pitch(t);
    u64 hash;
    TexEntry* e;
    TexEntry* victim = NULL;
    size_t bytes;
    int i;

    tile_dims(t, &w, &h);
    if (w * h * 4 > (int)sizeof(sDecodeBuf)) {
        h = sizeof(sDecodeBuf) / 4 / w;
    }
    *outW = w;
    *outH = h;
    if (sTexOverrideCount != 0 && (e = tex_override(t, w, h)) != NULL) {
        return e;
    }
    if (tc->entry != NULL && tc->entry->hash == tc->hash && tc->gen == sTmemGen && tc->tlutMode == tlutMode &&
        memcmp(&tc->tile, t, sizeof(Tile)) == 0) {
        tc->entry->lastUse = sFrame;
        return tc->entry;
    }

    double tt = stat_ms();
    // Content hash of the TMEM bytes the tile covers (+ palette for CI)
    bytes = pitch * h;
    if (t->tmem * 8 + bytes > 4096) {
        bytes = 4096 - t->tmem * 8;
    }
    hash = hash_bytes(&sTmem[t->tmem * 8], bytes, 0xCBF29CE484222325ull);
    hash = hash_bytes((const u8*)&w, sizeof(w), hash);
    hash = hash_bytes((const u8*)&h, sizeof(h), hash);
    hash = hash_bytes(&t->fmt, 1, hash);
    hash = hash_bytes(&t->siz, 1, hash);
    hash = hash_bytes((const u8*)&pitch, sizeof(pitch), hash);
    hash = hash_bytes(&t->masks, 1, hash);
    hash = hash_bytes(&t->maskt, 1, hash);
    hash = hash_bytes(&t->cms, 1, hash);
    hash = hash_bytes(&t->cmt, 1, hash);
    if (t->fmt == G_IM_FMT_CI || (t->fmt == G_IM_FMT_RGBA && tlutMode != 0 && t->siz < G_IM_SIZ_16b)) {
        hash = hash_bytes((const u8*)&tlutMode, sizeof(tlutMode), hash);
        if (t->siz == G_IM_SIZ_4b) {
            hash = hash_bytes((const u8*)&sTlut[t->palette << 4], 32, hash);
        } else {
            hash = hash_bytes((const u8*)sTlut, sizeof(sTlut), hash);
        }
    }

    e = &sTexCache[hash % TEX_CACHE_SIZE];
    for (i = 0; i < 8; i++) {
        TexEntry* c = &sTexCache[(hash + i) % TEX_CACHE_SIZE];
        if (c->tex != 0 && c->hash == hash) {
            e = c;
            goto found;
        }
        if (victim == NULL || c->tex == 0 || c->lastUse < victim->lastUse) {
            victim = c;
        }
    }
    e = victim;
    // Never re-specify a texture object the GPU may still be using this frame
    // (forces lima to flush all queued rendering): replace it
    if (e->tex != 0) {
        glDeleteTextures(1, &e->tex);
        if (sGL.tex[0] == e->tex) {
            sGL.tex[0] = 0;
        }
        if (sGL.tex[1] == e->tex) {
            sGL.tex[1] = 0;
        }
    }
    glGenTextures(1, &e->tex);
    sStats.uploads++;
    e->hash = hash;
    e->w = w;
    e->h = h;
    e->filter = e->wrapS = e->wrapT = 0;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            decode_texel(&sDecodeBuf[(y * w + x) * 4], t, pitch, tile_texel(x, t->masks, t->cms),
                         tile_texel(y, t->maskt, t->cmt), tlutMode);
        }
    }
    glActiveTexture(GL_TEXTURE0 + (tileIdx == sBatchTile ? 0 : 1));
    glBindTexture(GL_TEXTURE_2D, e->tex);
    sGL.tex[tileIdx == sBatchTile ? 0 : 1] = e->tex;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, sDecodeBuf);
    if (sTraceFrame != 0 && sFrame <= sTraceFrame && t->siz == G_IM_SIZ_32b && w == 160) {
        static int n;
        char path[64];
        FILE* f;
        snprintf(path, sizeof(path), "/tmp/tex32_%02d.rgba", n++);
        f = fopen(path, "wb");
        if (f != NULL) {
            fwrite(sDecodeBuf, 4, w * h, f);
            fclose(f);
        }
    }
found:
    sTTex += stat_ms() - tt;
    e->lastUse = sFrame;
    tc->gen = sTmemGen;
    tc->tile = *t;
    tc->tlutMode = tlutMode;
    tc->entry = e;
    tc->hash = e->hash;
    return e;
}

static int s3Point = -1; // <PREFIX>_3POINT: N64 3-point filter in the shader (slower)

/* Binds a tile's texture with its filter/wrap parameters; returns its size */
static void bind_tile(int unit, int tileIdx, float size[2]) {
    Tile* t = &rdp.tiles[tileIdx & 7];
    int w, h;
    TexEntry* e = tex_lookup(tileIdx, &w, &h);
    int point = (rdp.otherH & (3 << G_MDSFT_TEXTFILT)) == G_TF_POINT ||
                (rdp.otherH & (3 << G_MDSFT_CYCLETYPE)) == G_CYC_COPY;
    GLint filter = (point || s3Point) ? GL_NEAREST : GL_LINEAR;
    GLint ws = wrap_mode(t->cms, t->masks, w), wt = wrap_mode(t->cmt, t->maskt, h);

    if (sGL.tex[unit] != e->tex) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, e->tex);
        sGL.tex[unit] = e->tex;
    }
    if (e->filter != filter || e->wrapS != ws || e->wrapT != wt) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, e->tex);
        if (e->filter != filter) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            (e->mipmapped && filter == GL_LINEAR) ? GL_LINEAR_MIPMAP_LINEAR : filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        }
        if (e->wrapS != ws) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ws);
        }
        if (e->wrapT != wt) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wt);
        }
        e->filter = filter;
        e->wrapS = ws;
        e->wrapT = wt;
    }
    size[0] = (float)w;
    size[1] = (float)h;
}

static void set_uniform4(Shader* sh, GLint loc, int slot, const float v[4]) {
    if (memcmp(sh->cur[slot], v, sizeof(float) * 4) != 0) {
        glUniform4fv(loc, 1, v);
        memcpy(sh->cur[slot], v, sizeof(float) * 4);
    }
}

/* Per-tile uv transform u = s * scale + offset, cached until the tile changes */
static struct {
    int valid;
    float su, ou, sv, ov;
} sTileUv[8];

static void set_tile(int ti, const Tile* n) {
    if (memcmp(&rdp.tiles[ti], n, sizeof(Tile)) != 0) {
        if (ti == (sBatchTile & 7) || ti == ((sBatchTile + 1) & 7)) {
            flush_for(FR_TILE);
        }
        rdp.tiles[ti] = *n;
        sTileUv[ti].valid = 0;
    }
}

static inline void tile_uv_fast(int tileIdx, float s, float t, float* u, float* v) {
    int ti = tileIdx & 7;

    if (!sTileUv[ti].valid) {
        Tile* tl = &rdp.tiles[ti];
        int w, h;
        float fs = 1.0f, ft = 1.0f;

        tile_dims(tl, &w, &h);
        if (tl->shifts) {
            fs = tl->shifts <= 10 ? 1.0f / (1 << tl->shifts) : (float)(1 << (16 - tl->shifts));
        }
        if (tl->shiftt) {
            ft = tl->shiftt <= 10 ? 1.0f / (1 << tl->shiftt) : (float)(1 << (16 - tl->shiftt));
        }
        sTileUv[ti].su = fs / 32.0f / w;
        sTileUv[ti].ou = -(tl->uls / 4.0f) / w;
        sTileUv[ti].sv = ft / 32.0f / h;
        sTileUv[ti].ov = -(tl->ult / 4.0f) / h;
        sTileUv[ti].valid = 1;
    }
    *u = s * sTileUv[ti].su + sTileUv[ti].ou;
    *v = t * sTileUv[ti].sv + sTileUv[ti].ov;
}

/* Normalized texture coordinate for a 10.5 texel coordinate on a tile */
static void tile_uv(int tileIdx, float s, float t, float* u, float* v) {
    Tile* tl = &rdp.tiles[tileIdx & 7];
    int w, h;

    tile_dims(tl, &w, &h);
    if (tl->shifts) {
        s = tl->shifts <= 10 ? s / (float)(1 << tl->shifts) : s * (float)(1 << (16 - tl->shifts));
    }
    if (tl->shiftt) {
        t = tl->shiftt <= 10 ? t / (float)(1 << tl->shiftt) : t * (float)(1 << (16 - tl->shiftt));
    }
    *u = (s / 32.0f - tl->uls / 4.0f) / w;
    *v = (t / 32.0f - tl->ult / 4.0f) / h;
}

/* Last load into each TMEM start: reloading the same source data is a no-op,
   which keeps the texture cache key (sTmemGen) stable for repeated draws */
typedef struct {
    const void* src;
    u32 bytes, pitch, kind, gen;
} LoadKey;
static LoadKey sLastLoad[512];
static u32 sLoadGen = 1; // memos count only from the current generation: a
                         // bump forgets them all (was a memset of the table)

static int same_load(u32 tmem, const void* src, u32 bytes, u32 pitch, u32 kind) {
    LoadKey* k = &sLastLoad[tmem & 511];

    if (k->gen == sLoadGen && k->src == src && k->bytes == bytes && k->pitch == pitch && k->kind == kind) {
        return 1;
    }
    // A different load here also invalidates any memo it overlaps
    sLoadGen++;
    k->src = src;
    k->bytes = bytes;
    k->pitch = pitch;
    k->kind = kind;
    k->gen = sLoadGen;
    return 0;
}

/* TMEM copy from the game's u64-word data (byte i is at i ^ 7): aligned
   eight-byte groups in one byte swap */
static void copy_swapped(u8* dst, const u8* src, u32 n) {
    uintptr_t x = tex_xor(src);
    u32 i = 0;

    if (x == 0) {
        memcpy(dst, src, n);
        return;
    }
    if (x == 7 && ((uintptr_t)src & 7) == 0) {
        for (; i + 8 <= n; i += 8) {
            u64 w;
            memcpy(&w, src + i, 8);
            w = __builtin_bswap64(w);
            memcpy(dst + i, &w, 8);
        }
    } else if (x == 1 && ((uintptr_t)src & 1) == 0) {
        for (; i + 2 <= n; i += 2) {
            dst[i] = src[i + 1];
            dst[i + 1] = src[i];
        }
    }
    for (; i < n; i++) {
        dst[i] = rd8x(src + i, x);
    }
}
/* Where the current texture address was set (debugging bad addresses) */
static const Gfx* sTimgCmd;

/* The current texture image, or NULL when its address can't be real memory
   (in the first 16 MiB: an unresolved N64 address), logged with the
   command that set it so the display list can be found */
static const u8* texture_src(void) {
    static u32 sReported[64];
    static int sNumReported;
    const u8* src = pc_resolve_addr(rdp.timgAddr);
    int i;

    // Below 16 MiB: NULL-based (segment 0, an unset pointer). Asset segments
    // mapped at their segmented addresses (Star Fox: 0x01000000-0x0FFFFFFF)
    // and the program's memory are above.
    if ((src == NULL && rdp.timgAddr != 0) || (src != NULL && (uintptr_t)src < 0x01000000)) {
        // Each distinct address once (the skybox's NULL-based ones repeat
        // every frame)
        for (i = 0; i < sNumReported && sReported[i] != rdp.timgAddr; i++) {
        }
        if (i == sNumReported && sNumReported < 64) {
            sReported[sNumReported++] = rdp.timgAddr;
            printf("gfx: bad texture address %08x -> %p (G_SETTIMG at %p)%s\n", (unsigned)rdp.timgAddr, (void*)src,
                   (void*)sTimgCmd, src == NULL ? " (segment not set)" : "");
        }
        return NULL;
    }
    return src;
}

static void load_block(int tileIdx, u32 uls, u32 lrs, u32 dxt) {
    Tile* t = &rdp.tiles[tileIdx & 7];
    const u8* base = texture_src();
    const u8* src = base;
    u32 texels = lrs - uls + 1;
    u32 bytes = rdp.timgSiz == G_IM_SIZ_4b ? (texels + 1) / 2 : texels << (rdp.timgSiz - 1);
    u32 dst = t->tmem * 8;

    (void)dxt;
    if (src == NULL) {
        return;
    }
    aux_sync(src, ((lrs + 1) << rdp.timgSiz) / 2);
    if (sTraceFrame != 0 && sFrame == sTraceFrame) {
        printf("loadblock: tile %d src %p (align %u) texels %u siz %d tmem %d\n", tileIdx, (void*)src,
               (unsigned)((uintptr_t)src & 7), (unsigned)texels, rdp.timgSiz, t->tmem);
    }
    src += rdp.timgSiz == G_IM_SIZ_4b ? uls / 2 : uls << (rdp.timgSiz - 1);
    if (dst + bytes > 4096) {
        bytes = 4096 - dst;
    }
    if (same_load(t->tmem, src, bytes, 0, 1)) {
        return;
    }
    flush_for(FR_LOADBLOCK);
    sTmemSrc[t->tmem & 511] = base; // after the flush: queued draws still use the old texture
    copy_swapped(&sTmem[dst], src, bytes);
    sTmemGen++;
}

static void load_tile(int tileIdx, u32 uls, u32 ult, u32 lrs, u32 lrt) {
    Tile* t = &rdp.tiles[tileIdx & 7];
    const u8* src = texture_src();
    u32 x0 = uls >> 2, y0 = ult >> 2, x1 = lrs >> 2, y1 = lrt >> 2;
    u32 bpt2 = rdp.timgSiz == G_IM_SIZ_4b ? 1 : 2 << (rdp.timgSiz - 1); // bytes per texel * 2
    u32 rowBytes = ((x1 - x0 + 1) * bpt2 + 1) / 2;
    u32 srcPitch = (rdp.timgWidth * bpt2 + 1) / 2;
    u32 pitch = tile_pitch(t);
    u32 y;

    if (src == NULL) {
        return;
    }
    aux_sync(src + y0 * srcPitch, (y1 - y0 + 1) * srcPitch);
    if (same_load(t->tmem, src + y0 * srcPitch + (x0 * bpt2) / 2, rowBytes * (y1 - y0 + 1), pitch, 2)) {
        return;
    }
    flush_for(FR_LOADTILE);
    sTmemSrc[t->tmem & 511] = src; // after the flush: queued draws still use the old texture
    for (y = y0; y <= y1; y++) {
        const u8* s = src + y * srcPitch + (x0 * bpt2) / 2;
        u32 d = t->tmem * 8 + (y - y0) * pitch;
        if (d < 4096) {
            copy_swapped(&sTmem[d], s, d + rowBytes > 4096 ? 4096 - d : rowBytes);
        }
    }
    sTmemGen++;
}

static void load_tlut(int tileIdx, u32 count) {
    Tile* t = &rdp.tiles[tileIdx & 7];
    const u8* src = texture_src();
    u32 first = t->tmem >= 256 ? t->tmem - 256 : 0;
    u32 i;
    int changed = 0;

    if (src == NULL) {
        return;
    }
    // Compared, not memoized by source: palettes in game RAM change in place
    // (the pause map's pulsing room colour)
    for (i = 0; i < count && first + i < 256; i++) {
        uintptr_t x = tex_xor(src);
        u16 c = (rd8x(src + i * 2, x) << 8) | rd8x(src + i * 2 + 1, x);
        if (sTlut[first + i] != c) {
            if (!changed) {
                flush_for(FR_TLUT);
                changed = 1;
            }
            sTlut[first + i] = c;
        }
    }
    if (changed) {
        sTmemGen++;
    }
}

/* ---------------------------------------------------------------------------
 * Batching and draw state
 * ------------------------------------------------------------------------- */

#define VSTRIDE 21 // pos4 color4 uv0 2 uv1 2 fog1 prim4 env4
#define MAX_BATCH 3072
static float sBatch[MAX_BATCH * VSTRIDE];
static int sBatchVerts;

/* Batches go to the GPU through one streaming vertex buffer, written with
   unsynchronized maps: client-side arrays cost lima ~40% more per draw (a
   new upload buffer each time), and glBufferSubData on a buffer the pending
   job reads flushes the job. Frames rotate through VBO_RING buffers, so a
   buffer is rewritten only after the frame that last used it is on screen
   (EndFrame waits for the previous flip); orphaning one per frame instead
   had the kernel allocate and zero 4 MiB a frame. A frame that fills its
   buffer orphans it (rare). */
#define VBO_VERTS (24 * 1024) // 2 MiB; Clock Town is ~13-15k vertices a frame
#define VBO_RING 3
static GLuint sVboRing[VBO_RING], sVbo;
static int sVboFrame;
static int sVboNext = VBO_VERTS; // next free vertex; VBO_VERTS: orphan first
static PFNGLMAPBUFFERRANGEEXTPROC sMapRange;
static PFNGLUNMAPBUFFEROESPROC sUnmap;
static int sVboOn = -1;
static int sBatchScreen; // batch is screen-space (rects): full-screen viewport

/* ---------------------------------------------------------------------------
 * Auxiliary color images: an RGBA16 color image other than the 320-wide frame
 * (the pause menu renders Link into a 64x112 image, then copies it into the
 * texture the equipment page draws). Drawn 1:1 into a GL framebuffer of its
 * own and copied back to RAM (RGBA5551, N64 byte order) when a texture load
 * reads its memory.
 * ------------------------------------------------------------------------- */

#define MAX_AUX 8
#define AUX_MAX_W 320
#define AUX_H 240

typedef struct {
    u8* ram;
    u32 width;
    u32 rows; // rows drawn so far (scissor extent), the part copied back
    int dirty;
    u32 lastUse;
    GLuint tex, fbo, depth;
} AuxImage;

static AuxImage sAux[MAX_AUX];
static AuxImage* sAuxCur;   // target of the current color image (NULL: frame or none)
static AuxImage* sBatchAux; // target of the batched vertices

static void aux_bind(AuxImage* a) {
    glBindFramebuffer(GL_FRAMEBUFFER, a->fbo);
    S30Display_Unbind();
}

/* The auxiliary image at ram (width pixels per row); takes the least recently
   used slot for a new one */
static AuxImage* aux_get(u8* ram, u32 width) {
    AuxImage* a = &sAux[0];
    int i;

    if (ram == NULL) {
        return NULL;
    }
    for (i = 0; i < MAX_AUX; i++) {
        if (sAux[i].ram == ram && sAux[i].width == width) {
            sAux[i].lastUse = sFrame;
            return &sAux[i];
        }
        if (sAux[i].lastUse < a->lastUse) {
            a = &sAux[i];
        }
    }
    if (a->fbo == 0) {
        glGenTextures(1, &a->tex);
        glGenRenderbuffers(1, &a->depth);
        glGenFramebuffers(1, &a->fbo);
    }
    if (a->width != width) {
        glBindTexture(GL_TEXTURE_2D, a->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, AUX_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindRenderbuffer(GL_RENDERBUFFER, a->depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, AUX_H);
        glBindFramebuffer(GL_FRAMEBUFFER, a->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, a->tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, a->depth);
        S30Display_Unbind();
        gl_cache_reset();
    }
    a->ram = ram;
    a->width = width;
    a->rows = 0;
    a->dirty = 0;
    a->lastUse = sFrame;
    return a;
}

/* Copy drawn auxiliary images overlapping [src, src + bytes) back to RAM */
static void aux_sync(const u8* src, u32 bytes) {
    static u8 px[AUX_MAX_W * AUX_H * 4];
    int i;

    for (i = 0; i < MAX_AUX; i++) {
        AuxImage* a = &sAux[i];
        u32 x, y;

        if (!a->dirty || src + bytes <= a->ram || src >= a->ram + a->width * a->rows * 2) {
            continue;
        }
        flush_for(FR_AUXSYNC);
        aux_bind(a);
        glReadPixels(0, AUX_H - a->rows, a->width, a->rows, GL_RGBA, GL_UNSIGNED_BYTE, px);
        // GL rows run bottom-up
        for (y = 0; y < a->rows; y++) {
            const u8* p = px + (a->rows - 1 - y) * a->width * 4;
            for (x = 0; x < a->width; x++, p += 4) {
                u16 c = ((p[0] >> 3) << 11) | ((p[1] >> 3) << 6) | ((p[2] >> 3) << 1) | (p[3] >= 128);
                u8* d = a->ram + (y * a->width + x) * 2;
                *(u8*)((uintptr_t)d ^ 7) = c >> 8;
                *(u8*)((uintptr_t)(d + 1) ^ 7) = c & 0xFF;
            }
        }
        a->dirty = 0;
        sLoadGen++;
    }
}

static GLuint sBgTex; // while drawing an S2DEX background: its texture replaces tile 0

static void apply_state(void) {
    u32 cyc = rdp.otherH & (3 << G_MDSFT_CYCLETYPE);
    u32 l = rdp.otherL;
    u32 flags = 0;
    int blend = 0;
    u32 bl = cyc == G_CYC_2CYCLE ? (l >> 16) : (l >> 18);
    u32 m1a = (bl >> 12) & 3, m1b = (bl >> 8) & 3, m2a = (bl >> 4) & 3, m2b = bl & 3;
    Shader* sh;
    int i;

    if (cyc == G_CYC_FILL) {
        flags |= SF_FILL;
    } else if (cyc == G_CYC_COPY) {
        flags |= SF_COPY;
        // Copy mode has alpha compare too: the classic cutout sprite (Mario
        // Kart 64's HUD digits)
        if ((l & 3) == G_AC_THRESHOLD) {
            flags |= SF_ALPHA_THRESH;
        }
    } else {
        if (cyc == G_CYC_2CYCLE) {
            flags |= SF_2CYC;
            // Cycle 1 of the blender: fog color by shade alpha into the pixel
            if (((l >> 30) & 3) == 3 && ((l >> 26) & 3) == 2) {
                flags |= SF_FOG;
            }
        }
        // Alpha compare: blended draws already fade low alpha out; only
        // unblended (cutout) draws need the threshold discard
        if ((l & 3) == G_AC_THRESHOLD && !((l & FORCE_BL) && m2a == 1)) {
            flags |= SF_ALPHA_THRESH;
        }
        if ((l & CVG_X_ALPHA) && (l & ALPHA_CVG_SEL)) {
            flags |= SF_TEX_EDGE;
        } else if ((l & CVG_X_ALPHA) && (l & AA_EN)) {
            // Blended with coverage from alpha (Mario Kart 64's item and
            // smoke sprites): transparent texels write no depth
            flags |= SF_CVG_ALPHA;
        }
        // Last blender cycle reads memory with (1 - a): alpha blending
        if ((l & FORCE_BL) && m2a == 1) {
            blend = 1;
        }
    }
    (void)m1a;

    if (s3Point < 0) {
        s3Point = S30_Getenv("3POINT") != NULL;
    }
    if (s3Point && cyc < G_CYC_COPY && (rdp.otherH & (3 << G_MDSFT_TEXTFILT)) != G_TF_POINT) {
        flags |= SF_3POINT;
    }
    if (sBatchScreen && S30_Getenv("DBG_ALPHA")) {
        flags = (flags & ~(SF_ALPHA_THRESH | SF_TEX_EDGE)) | SF_DBG_ALPHA;
        blend = 0;
    }
    if (cyc == G_CYC_1CYCLE) {
        // Second-cycle terms unused: one program for all of them
        sh = get_shader(rdp.combW0 & COMB1_W0, rdp.combW1 & COMB1_W1, flags);
    } else {
        sh = get_shader(rdp.combW0, rdp.combW1, flags);
    }
    if (sGL.prog != sh->prog) {
        glUseProgram(sh->prog);
        sGL.prog = sh->prog;
    }
    sCurShader = sh;
    set_uniform4(sh, sh->uFog, 2, rdp.fog);
    {
        // Alpha compare threshold (see the shader): at least 1/255
        float b[4] = { rdp.blend[0], rdp.blend[1], rdp.blend[2], rdp.blend[3] };

        if (b[3] < 1.0f / 255.0f) {
            b[3] = 1.0f / 255.0f;
        }
        set_uniform4(sh, sh->uBlend, 3, b);
    }
    set_uniform4(sh, sh->uFill, 4, rdp.fill);
    if (sh->curLod != rdp.primLod) {
        glUniform1f(sh->uPrimLod, rdp.primLod);
        sh->curLod = rdp.primLod;
    }
    if (sh->uNoise >= 0 && sh->curNoise != sFrame) {
        // A new offset each frame (an LCG step of the frame number)
        u32 r = sFrame * 1103515245u + 12345u;

        glUniform2f(sh->uNoise, (float)(r & 0x3F) / 64.0f, (float)((r >> 6) & 0x3F) / 64.0f);
        sh->curNoise = sFrame;
    }

    for (i = 0; i < 2; i++) {
        float size[2];
        if (i == 0 && sBgTex != 0 && sh->usesTex[0]) {
            // S2DEX background: its own texture, not a TMEM tile
            if (sGL.tex[0] != sBgTex) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, sBgTex);
                sGL.tex[0] = sBgTex;
            }
        } else if (sh->usesTex[i]) {
            bind_tile(i, sBatchTile + i, size);
            if ((flags & SF_3POINT) && memcmp(sh->curSize[i], size, sizeof(size)) != 0) {
                glUniform2fv(i ? sh->uTexSize1 : sh->uTexSize0, 1, size);
                memcpy(sh->curSize[i], size, sizeof(size));
            }
        }
    }

    if (blend && sBatchScreen && S30_Getenv("DBG_NOBLEND")) {
        blend = 0;
    }
    {
        // Color factors for the blend; the 2D layer also keeps premultiplied
        // coverage in alpha so it composites over the 3D image
        GLenum src = GL_SRC_ALPHA, dst = GL_ONE_MINUS_SRC_ALPHA;

        if (m2b == 2) {
            src = m1b == 3 ? GL_ZERO : GL_SRC_ALPHA;
            dst = GL_ONE;
        } else if (m1b == 3 && m2b == 0) {
            src = GL_ZERO; // a = 0: memory only
            dst = GL_ONE;
        }
        if (sBatchAux != NULL) {
            // RGBA5551 alpha is the RDP's coverage bit: full for every draw
            gl_enable(GL_BLEND, 1, &sGL.blend);
            if (blend) {
                gl_blend_func(src, dst, GL_CONSTANT_ALPHA, GL_ZERO);
            } else {
                gl_blend_func(GL_ONE, GL_ZERO, GL_CONSTANT_ALPHA, GL_ZERO);
            }
        } else if (sBatchScreen) {
            gl_enable(GL_BLEND, 1, &sGL.blend);
            if (blend) {
                gl_blend_func(src, dst, dst == GL_ONE ? GL_ZERO : GL_ONE, dst == GL_ONE ? GL_ONE : dst);
            } else {
                gl_blend_func(GL_ONE, GL_ZERO, GL_CONSTANT_ALPHA, GL_ZERO); // opaque: alpha 1
            }
        } else if (blend) {
            gl_enable(GL_BLEND, 1, &sGL.blend);
            gl_blend_func(src, dst, src, dst);
        } else {
            gl_enable(GL_BLEND, 0, &sGL.blend);
        }
    }

    // Depth (the 2D layer has none)
    gl_enable(GL_DEPTH_TEST, !sBatchScreen && (rsp.geom & G_ZBUFFER) && (l & Z_CMP) && cyc < G_CYC_COPY,
              &sGL.depthTest);
    {
        int mask = !sBatchScreen && (l & Z_UPD) && cyc < G_CYC_COPY;
        if (sGL.depthMask != mask) {
            glDepthMask(mask ? GL_TRUE : GL_FALSE);
            sGL.depthMask = mask;
        }
    }
    gl_enable(GL_POLYGON_OFFSET_FILL, (l & ZMODE_DEC) == ZMODE_DEC, &sGL.polyOfs);

    // Culling: N64 front faces are counter-clockwise in GL's y-up NDC
    gl_enable(GL_CULL_FACE, 0, &sGL.cull); // culled per triangle in tri()

    // Viewport and scissor. Auxiliary images are 1:1 with N64 pixels; the 2D
    // layer is 1:1 with WIDE_PAD margins; the frame is FB_SCALE with the same
    // margins (see WIDE_PAD). Full-width scissors open the whole width.
    if (sBatchAux != NULL) {
        if (sBatchScreen) {
            gl_viewport(0, 0, sBatchAux->width, AUX_H);
        } else {
            gl_viewport((int)rsp.vpX, (int)(AUX_H - rsp.vpY - rsp.vpH), (int)rsp.vpW, (int)rsp.vpH);
        }
        gl_enable(GL_SCISSOR_TEST, 1, &sGL.scissor);
        gl_scissor(rdp.scX, AUX_H - rdp.scY - rdp.scH, rdp.scW, rdp.scH);
    } else {
        float s = sBatchScreen ? 1.0f : FB_SCALE, w = sBatchScreen ? S30_2D_W : S30_FB_W;
        int m = sCfg.overscan;
        int fullScissor = rdp.scX <= m && rdp.scX + rdp.scW >= SCREEN_W - m;
        int fullScissorY = m > 0 && fullScissor && rdp.scY <= m && rdp.scY + rdp.scH >= SCREEN_H - m;
        int scY = fullScissorY ? 0 : rdp.scY, scH = fullScissorY ? SCREEN_H : rdp.scH;

        if (sBatchScreen) {
            gl_viewport(0, 0, S30_2D_W, S30_2D_H);
        } else if (sWide && rsp.vpX <= 0.0f && rsp.vpX + rsp.vpW >= SCREEN_W) {
            gl_viewport(0, (int)((SCREEN_H - rsp.vpY - rsp.vpH) * s), S30_FB_W, (int)(rsp.vpH * s));
        } else {
            // Rounded inward: the 2D layer's edge pixels over the 4:3 window are
            // blended (sharp bilinear), so 3D just outside would show through
            // them (a prerendered room's background over its room geometry)
            int x0 = (int)ceilf((rsp.vpX + WIDE_PAD) * s), x1 = (int)floorf((rsp.vpX + rsp.vpW + WIDE_PAD) * s);
            gl_viewport(x0, (int)((SCREEN_H - rsp.vpY - rsp.vpH) * s), x1 - x0, (int)(rsp.vpH * s));
        }
        gl_enable(GL_SCISSOR_TEST, 1, &sGL.scissor);
        gl_scissor(fullScissor ? 0 : (int)((rdp.scX + WIDE_PAD) * s), (int)((SCREEN_H - scY - scH) * s),
                   fullScissor ? (int)w : (int)(rdp.scW * s + 0.5f), (int)(scH * s + 0.5f));
    }
}

static void flush(void) {
    double tf;

    if (sBatchVerts == 0) {
        return;
    }
    if (sFlushReason >= 0) {
        sFlushWhy[sFlushReason]++;
    }
    tf = stat_ms();
    if (sBatchAux != NULL) {
        aux_bind(sBatchAux);
        if (rdp.scY + rdp.scH > (int)sBatchAux->rows) {
            sBatchAux->rows = rdp.scY + rdp.scH < AUX_H ? rdp.scY + rdp.scH : AUX_H;
        }
        sBatchAux->dirty = 1;
    } else if (sBatchScreen) {
        S30Display_Bind2D();
    } else if (S30Display_Bind3D()) {
        gl_cache_reset(); // the 2D composite changed GL state
        sStats.switches++;
    }
    apply_state();
    if (sVboOn < 0) {
        const char* ext = (const char*)glGetString(GL_EXTENSIONS);

        sMapRange = (PFNGLMAPBUFFERRANGEEXTPROC)eglGetProcAddress("glMapBufferRangeEXT");
        sUnmap = (PFNGLUNMAPBUFFEROESPROC)eglGetProcAddress("glUnmapBufferOES");
        sVboOn = ext != NULL && strstr(ext, "GL_EXT_map_buffer_range") != NULL && sMapRange != NULL &&
                 sUnmap != NULL && S30_Getenv("NOVBO") == NULL;
        if (sVboOn) {
            int r;
            glGenBuffers(VBO_RING, sVboRing);
            for (r = 0; r < VBO_RING; r++) {
                glBindBuffer(GL_ARRAY_BUFFER, sVboRing[r]);
                glBufferData(GL_ARRAY_BUFFER, VBO_VERTS * VSTRIDE * 4, NULL, GL_STREAM_DRAW);
            }
            sVbo = sVboRing[0];
            sVboNext = 0;
            sAttribsSet = 0;
        }
        printf("gfx: vertices %s\n", sVboOn ? "through a streaming buffer" : "as client arrays");
    }
    if (!sAttribsSet) {
        // The 2D composite and the display blit reuse attribute slots (and
        // bind no buffer), so this reruns after them
        static const struct {
            int idx, n, ofs;
        } attrs[] = {
            { ATTR_POS, 4, 0 }, { ATTR_COLOR, 4, 4 }, { ATTR_UV0, 2, 8 },   { ATTR_UV1, 2, 10 },
            { ATTR_FOG, 1, 12 }, { ATTR_PRIM, 4, 13 }, { ATTR_ENV, 4, 17 },
        };
        int a;
        glBindBuffer(GL_ARRAY_BUFFER, sVboOn ? sVbo : 0);
        for (a = 0; a < 7; a++) {
            glEnableVertexAttribArray(attrs[a].idx);
            glVertexAttribPointer(attrs[a].idx, attrs[a].n, GL_FLOAT, GL_FALSE, VSTRIDE * 4,
                                  sVboOn ? (const void*)(uintptr_t)(attrs[a].ofs * 4) : (const void*)(sBatch + attrs[a].ofs));
        }
        sAttribsSet = 1;
    }
    {
        double td = stat_ms();
        if (sVboOn) {
            void* dst;

            if (sVboNext + sBatchVerts > VBO_VERTS) {
                glBufferData(GL_ARRAY_BUFFER, VBO_VERTS * VSTRIDE * 4, NULL, GL_STREAM_DRAW);
                sVboNext = 0;
            }
            dst = sMapRange(GL_ARRAY_BUFFER, sVboNext * VSTRIDE * 4, sBatchVerts * VSTRIDE * 4,
                            GL_MAP_WRITE_BIT_EXT | GL_MAP_UNSYNCHRONIZED_BIT_EXT | GL_MAP_INVALIDATE_RANGE_BIT_EXT);
            if (dst != NULL) {
                memcpy(dst, sBatch, sBatchVerts * VSTRIDE * 4);
                sUnmap(GL_ARRAY_BUFFER);
                glDrawArrays(GL_TRIANGLES, sVboNext, sBatchVerts);
            }
            sVboNext += sBatchVerts;
        } else {
            glDrawArrays(GL_TRIANGLES, 0, sBatchVerts);
        }
        sTDraw += stat_ms() - td;
    }
    sTFlush += stat_ms() - tf;
    sStats.draws++;
    {
        // <PREFIX>_TRACECOMB=<combiner w0 hex>: log draws using that combiner
        static int comb = -1;
        static u32 lastLog;
        if (comb < 0) {
            comb = S30_Getenv("TRACECOMB") ? (int)strtol(S30_Getenv("TRACECOMB"), NULL, 16) : 0;
        }
        if (comb != 0 && rdp.combW0 == (u32)comb && sFrame - lastLog >= 10) {
            lastLog = sFrame;
            printf("comb-draw f%u: v %d scr %d H %06x L %08x geom %06x tex %d tile %d prim %.2f,%.2f,%.2f,%.2f "
                   "v0 %.1f,%.1f,%.1f,%.1f v1 %.1f,%.1f v2 %.1f,%.1f uv %.2f,%.2f\n",
                   sFrame, sBatchVerts, sBatchScreen, rdp.otherH, rdp.otherL, rsp.geom, rsp.texOn, rsp.texTile,
                   rdp.prim[0], rdp.prim[1], rdp.prim[2], rdp.prim[3], sBatch[0], sBatch[1], sBatch[2], sBatch[3],
                   sBatch[VSTRIDE], sBatch[VSTRIDE + 1], sBatch[2 * VSTRIDE], sBatch[2 * VSTRIDE + 1], sBatch[8],
                   sBatch[9]);
        }
    }
    if (sTraceFrame != 0 && sFrame == sTraceFrame) {
        Tile* t = &rdp.tiles[rsp.texTile & 7];
        int vi;
        float umin = 1e9f, umax = -1e9f, vmin = 1e9f, vmax = -1e9f;
        for (vi = 0; vi < sBatchVerts; vi++) {
            float u = sBatch[vi * VSTRIDE + 8], v = sBatch[vi * VSTRIDE + 9];
            umin = u < umin ? u : umin;
            umax = u > umax ? u : umax;
            vmin = v < vmin ? v : vmin;
            vmax = v > vmax ? v : vmax;
        }
        printf("   uvrange %.3f..%.3f %.3f..%.3f tilesize %d,%d-%d,%d texscale %u,%u\n", umin, umax, vmin, vmax,
               t->uls / 4, t->ult / 4, t->lrs / 4, t->lrt / 4, rsp.texScaleS, rsp.texScaleT);
        printf("draw %u: v %d scr %d comb %06x:%08x H %06x L %08x geom %06x tex %d tile %d fmt %d/%d %dx%d "
               "line %d tmem %d cm %d/%d mask %d/%d shift %d/%d prim %.2f,%.2f,%.2f,%.2f env %.2f,%.2f,%.2f,%.2f "
               "v0 %.1f,%.1f,%.1f,%.1f c %.2f,%.2f,%.2f,%.2f uv %.3f,%.3f\n",
               sStats.draws, sBatchVerts, sBatchScreen, rdp.combW0, rdp.combW1, rdp.otherH, rdp.otherL, rsp.geom,
               rsp.texOn, rsp.texTile, t->fmt, t->siz, ((t->lrs - t->uls) >> 2) + 1, ((t->lrt - t->ult) >> 2) + 1,
               t->line, t->tmem, t->cms, t->cmt, t->masks, t->maskt, t->shifts, t->shiftt, rdp.prim[0], rdp.prim[1],
               rdp.prim[2], rdp.prim[3], rdp.env[0], rdp.env[1], rdp.env[2], rdp.env[3], sBatch[0], sBatch[1],
               sBatch[2], sBatch[3], sBatch[4], sBatch[5], sBatch[6], sBatch[7], sBatch[8], sBatch[9]);
        printf("   glerr %x timg %08x fmt %d/%d\n", glGetError(), rdp.timgAddr, rdp.timgFmt, rdp.timgSiz);
        printf("   blend %.2f,%.2f,%.2f,%.2f fog %.2f,%.2f,%.2f,%.2f sc %d,%d %dx%d vp %.0f,%.0f %.0fx%.0f\n",
               rdp.blend[0], rdp.blend[1], rdp.blend[2], rdp.blend[3], rdp.fog[0], rdp.fog[1], rdp.fog[2],
               rdp.fog[3], rdp.scX, rdp.scY, rdp.scW, rdp.scH, rsp.vpX, rsp.vpY, rsp.vpW, rsp.vpH);
        printf("   tmem@%d: %02x %02x %02x %02x %02x %02x %02x %02x  tlut: %04x %04x %04x %04x  tile1 fmt %d/%d tmem %d\n",
               t->tmem * 8, sTmem[t->tmem * 8], sTmem[t->tmem * 8 + 1], sTmem[t->tmem * 8 + 2],
               sTmem[t->tmem * 8 + 3], sTmem[t->tmem * 8 + 4], sTmem[t->tmem * 8 + 5], sTmem[t->tmem * 8 + 6],
               sTmem[t->tmem * 8 + 7], sTlut[0], sTlut[1], sTlut[2], sTlut[3], rdp.tiles[(rsp.texTile + 1) & 7].fmt,
               rdp.tiles[(rsp.texTile + 1) & 7].siz, rdp.tiles[(rsp.texTile + 1) & 7].tmem);
    }
    sBatchVerts = 0;
}

/* Draw commands go to the frame or an auxiliary image, never the z-buffer */
static int drawing_enabled(void) {
    return rdp.cimg != rdp.zimg && (rdp.cimgIsFrame || sAuxCur != NULL);
}

/* Vertices about to be added: a batch is one draw, so it needs one target,
   one space (screen or 3D) and one render tile */
static void begin_batch_tile(int screen, int tile) {
    if (sBatchScreen != screen || sBatchAux != sAuxCur || sBatchTile != tile || sBatchVerts + 6 > MAX_BATCH) {
        flush_for(sBatchVerts + 6 > MAX_BATCH ? FR_BATCH_FULL
                  : sBatchTile != tile         ? FR_BATCH_TILE
                                               : FR_BATCH_SPACE);
    }
    sBatchScreen = screen;
    sBatchAux = sAuxCur;
    sBatchTile = tile;
}

static void begin_batch(int screen) {
    begin_batch_tile(screen, rsp.texTile);
}

static void push_vertex(float x, float y, float z, float w, const float col[4], float u0, float v0, float u1,
                        float v1, float fog) {
    float* d = &sBatch[sBatchVerts * VSTRIDE];

    d[0] = x;
    d[1] = y;
    d[2] = z;
    d[3] = w;
    d[4] = col[0];
    d[5] = col[1];
    d[6] = col[2];
    d[7] = col[3];
    d[8] = u0;
    d[9] = v0;
    d[10] = u1;
    d[11] = v1;
    d[12] = fog;
    memcpy(&d[13], rdp.prim, sizeof(rdp.prim));
    memcpy(&d[17], rdp.env, sizeof(rdp.env));
    sBatchVerts++;
}

/* ---------------------------------------------------------------------------
 * RSP: vertices and triangles
 * ------------------------------------------------------------------------- */

static void handle_vtx(u32 w0, u32 w1) {
    u32 n = (w0 >> 12) & 0xFF;
    u32 dst = ((w0 >> 1) & 0x7F) - n;
    const Vtx* src = pc_resolve_addr(w1);
    float(*m)[4];
    u32 i;

    if (src == NULL || dst + n > MAX_VERTS) {
        return;
    }
    if (rsp.mvpDirty) {
        mtx_mul(rsp.mvp, rsp.mv[rsp.mvTop], rsp.p);
        rsp.mvpDirty = 0;
    }
    if (rsp.lightsDirty && (rsp.geom & (G_LIGHTING | G_TEXTURE_GEN))) {
        update_lights();
    }
    m = rsp.mvp;
    for (i = 0; i < n; i++) {
        const Vtx* v = &src[i];
        RspVtx* d = &rsp.vtx[dst + i];
        float x = v->v.ob[0], y = v->v.ob[1], z = v->v.ob[2];

        d->x = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
        d->y = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
        d->z = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
        d->w = x * m[0][3] + y * m[1][3] + z * m[2][3] + m[3][3];
        if (sWide && rsp.vpX <= 0.0f && rsp.vpX + rsp.vpW >= SCREEN_W) {
            d->x *= WIDE_XSCALE; // full-screen 3D: widescreen field of view
        }
        d->clip = (d->x < -d->w) | ((d->x > d->w) << 1) | ((d->y < -d->w) << 2) | ((d->y > d->w) << 3);

        d->u = (float)(v->v.tc[0] * rsp.texScaleS >> 16);
        d->v = (float)(v->v.tc[1] * rsp.texScaleT >> 16);

        if (rsp.geom & G_LIGHTING) {
            float nx = v->n.n[0] / 127.0f, ny = v->n.n[1] / 127.0f, nz = v->n.n[2] / 127.0f;
            float r = rsp.ambient[0], g = rsp.ambient[1], b = rsp.ambient[2];
            int l;

            for (l = 0; l < rsp.numLights; l++) {
                float dt = nx * rsp.lights[l].mdir[0] + ny * rsp.lights[l].mdir[1] + nz * rsp.lights[l].mdir[2];
                if (dt > 0.0f) {
                    r += dt * rsp.lights[l].col[0];
                    g += dt * rsp.lights[l].col[1];
                    b += dt * rsp.lights[l].col[2];
                }
            }
            d->r = r > 1.0f ? 1.0f : r;
            d->g = g > 1.0f ? 1.0f : g;
            d->b = b > 1.0f ? 1.0f : b;
            if (rsp.geom & G_TEXTURE_GEN) {
                float dx = nx * rsp.mlookat[0][0] + ny * rsp.mlookat[0][1] + nz * rsp.mlookat[0][2];
                float dy = nx * rsp.mlookat[1][0] + ny * rsp.mlookat[1][1] + nz * rsp.mlookat[1][2];
                if (rsp.geom & G_TEXTURE_GEN_LINEAR) {
                    dx = acosf(-dx) / (float)M_PI * 2.0f - 1.0f;
                    dy = acosf(-dy) / (float)M_PI * 2.0f - 1.0f;
                }
                d->u = (dx + 1.0f) / 4.0f * rsp.texScaleS;
                d->v = (dy + 1.0f) / 4.0f * rsp.texScaleT;
            }
        } else {
            d->r = v->v.cn[0] / 255.0f;
            d->g = v->v.cn[1] / 255.0f;
            d->b = v->v.cn[2] / 255.0f;
        }
        d->a = v->v.cn[3] / 255.0f;
        if (rsp.geom & G_FOG) {
            // Fog factor replaces shade alpha, as on the RSP
            float w = fabsf(d->w) < 0.001f ? 0.001f : d->w;
            float winv = w < 0.0f ? 32767.0f : 1.0f / w; // RSP: behind the eye
            float f = d->z * winv * rsp.fogMul + rsp.fogOfs;
            f = f < 0.0f ? 0.0f : f > 255.0f ? 255.0f : f;
            d->a = f / 255.0f;
        }
        // F3DEX2 NoN: no near-plane clipping (first-person items between the
        // eye and the near plane still draw), the RDP clamping each pixel's
        // depth. Done in tri() (split at the plane): clamping this vertex
        // alone would pull the depth of a whole large triangle (ground
        // crossing the near plane) forward over everything
        if (d->w > 0.0f && d->z < -d->w) {
            d->clip |= CLIP_NEAR;
        }
    }
}

/* Depth layout: NDC [-1, -1 + NEAR_SLICE) holds what is in front of the near
   plane (NoN draws it; the RDP clamps its depth), [-1 + NEAR_SLICE, 1] the
   rest. Inside the slice depth goes by -w/z (1 at the plane, towards 0 at
   the eye), so close-up pieces (a first-person item and the hand holding
   it) keep their order instead of all fighting at one depth; the far range
   is mapped linearly in clip space (exact interpolation) and meets the
   slice at the plane. */
#define NEAR_SLICE (1.0f / 64.0f)

static float depth_z(const RspVtx* p, int inNear) {
    if (inNear) {
        float f = p->z < -p->w ? -p->w / p->z : 1.0f;
        return p->w * (-1.0f + NEAR_SLICE * f);
    }
    return p->z * (1.0f - NEAR_SLICE / 2.0f) + p->w * (NEAR_SLICE / 2.0f);
}

/* An NDC depth from the game (depth probes) in the same layout */
static float depth_ndc(float z) {
    return z < -1.0f ? -1.0f + NEAR_SLICE * (-1.0f / z) : z * (1.0f - NEAR_SLICE / 2.0f) + NEAR_SLICE / 2.0f;
}

/* One vertex into the batch; nearest: in front of the near plane */
static void emit_vertex(const RspVtx* p, int nearest) {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    float col[4] = { p->r, p->g, p->b, p->a };

    if (rsp.texOn) {
        tile_uv_fast(rsp.texTile, p->u, p->v, &u0, &v0);
        tile_uv_fast(rsp.texTile + 1, p->u, p->v, &u1, &v1);
    }
    push_vertex(p->x, p->y, depth_z(p, nearest), p->w, col, u0, v0, u1, v1, (rsp.geom & G_FOG) ? p->a : 0.0f);
}

/* A polygon (3-4 vertices, in order) as a fan */
static void emit_polygon(const RspVtx* p, int n, int nearest) {
    int i;

    for (i = 1; i + 1 < n; i++) {
        begin_batch(0);
        emit_vertex(&p[0], nearest);
        emit_vertex(&p[i], nearest);
        emit_vertex(&p[i + 1], nearest);
    }
}

/* Point of the edge a-b on the near plane (z = -w), every attribute linear
   in clip space */
static RspVtx near_point(const RspVtx* a, const RspVtx* b) {
    float da = a->z + a->w, db = b->z + b->w, t = da / (da - db);
    RspVtx r;

    r.x = a->x + (b->x - a->x) * t;
    r.y = a->y + (b->y - a->y) * t;
    r.z = a->z + (b->z - a->z) * t;
    r.w = a->w + (b->w - a->w) * t;
    r.r = a->r + (b->r - a->r) * t;
    r.g = a->g + (b->g - a->g) * t;
    r.b = a->b + (b->b - a->b) * t;
    r.a = a->a + (b->a - a->a) * t;
    r.u = a->u + (b->u - a->u) * t;
    r.v = a->v + (b->v - a->v) * t;
    r.clip = 0;
    return r;
}

static void tri(int i0, int i1, int i2) {
    const RspVtx* v[3] = { &rsp.vtx[i0 % MAX_VERTS], &rsp.vtx[i1 % MAX_VERTS], &rsp.vtx[i2 % MAX_VERTS] };
    int k, near;

    sStats.tris++;
    if (!drawing_enabled()) {
        sStats.trisSkipped++;
        return;
    }
    if (v[0]->clip & v[1]->clip & v[2]->clip & ~CLIP_NEAR) {
        sStats.trisClipped++;
        return; // all outside the same plane
    }
    if (rsp.geom & G_CULL_BOTH) {
        // Face culling here rather than in GL, so cull mode changes don't end
        // the batch. The sign of det(x, y, w) is the facing in window space
        // (counter-clockwise front in GL's y-up NDC), also for triangles
        // crossing w = 0 (Olano & Greer)
        float det = v[0]->x * (v[1]->y * v[2]->w - v[2]->y * v[1]->w) -
                    v[0]->y * (v[1]->x * v[2]->w - v[2]->x * v[1]->w) +
                    v[0]->w * (v[1]->x * v[2]->y - v[2]->x * v[1]->y);

        if (((rsp.geom & G_CULL_BACK) && det <= 0.0f) || ((rsp.geom & G_CULL_FRONT) && det >= 0.0f)) {
            sStats.trisClipped++;
            return;
        }
    }
    near = (v[0]->clip & v[1]->clip & v[2]->clip & CLIP_NEAR) != 0;
    if (near || !((v[0]->clip | v[1]->clip | v[2]->clip) & CLIP_NEAR) || v[0]->w <= 0.0f || v[1]->w <= 0.0f ||
        v[2]->w <= 0.0f) {
        // Wholly in front of the near plane: in the near slice (see
        // depth_z); wholly behind it: as is. (Crossing it with a vertex
        // behind the eye: as is, GL clips.)
        begin_batch(0);
        for (k = 0; k < 3; k++) {
            emit_vertex(v[k], near);
        }
        return;
    }
    {
        // Crossing the near plane (a first-person item, a ground triangle at
        // the camera): split there. The far part keeps its depth, the near
        // part goes in the near slice, in front of everything else.
        RspVtx farP[4], nearP[4];
        int nf = 0, nn = 0;

        for (k = 0; k < 3; k++) {
            const RspVtx* a = v[k];
            const RspVtx* b = v[(k + 1) % 3];
            int aNear = (a->clip & CLIP_NEAR) != 0, bNear = (b->clip & CLIP_NEAR) != 0;

            if (aNear) {
                nearP[nn++] = *a;
            } else {
                farP[nf++] = *a;
            }
            if (aNear != bNear) {
                RspVtx m = near_point(a, b);
                farP[nf++] = m;
                nearP[nn++] = m;
            }
        }
        emit_polygon(farP, nf, 0);
        emit_polygon(nearP, nn, 1);
    }
}

/* N64 Mtx: s15.16 fixed point, the integer halves then the fraction halves,
   each stored where the union names it (u.i, u.f) */
static void mtx_from_fixed(float m[4][4], const void* mtx) {
    const u16* i = mtx;       // u16 i[4][4]
    const u16* f = i + 16;    // u16 f[4][4]
    int e;

    for (e = 0; e < 16; e++) {
        m[e / 4][e % 4] = (s32)(((u32)i[e] << 16) | f[e]) / 65536.0f;
    }
}

static void handle_mtx(u32 w0, u32 w1) {
    Mtx* mtx = pc_resolve_addr(w1);
    u8 p = w0 & 0xFF;
    float fixed[4][4];
    float(*m)[4];

    if (mtx == NULL) {
        return;
    }
    if (sCfg.floatMtx) {
        m = (float(*)[4])mtx->m;
    } else {
        mtx_from_fixed(fixed, mtx);
        m = fixed;
    }
    // F3DEX2: the PUSH bit is inverted on the wire (G_MTX_PUSH = 0x01 ^ 0x01)
    if (p & G_MTX_PROJECTION) {
        if (p & G_MTX_LOAD) {
            memcpy(rsp.p, m, sizeof(rsp.p));
        } else {
            mtx_mul(rsp.p, m, rsp.p);
        }
    } else {
        if (!(p & G_MTX_PUSH) && rsp.mvTop < MTX_STACK - 1) {
            memcpy(rsp.mv[rsp.mvTop + 1], rsp.mv[rsp.mvTop], sizeof(rsp.mv[0]));
            rsp.mvTop++;
        }
        if (p & G_MTX_LOAD) {
            memcpy(rsp.mv[rsp.mvTop], m, sizeof(rsp.mv[0]));
        } else {
            mtx_mul(rsp.mv[rsp.mvTop], m, rsp.mv[rsp.mvTop]);
        }
        rsp.lightsDirty = 1;
    }
    rsp.mvpDirty = 1;
}

static void handle_movemem(u32 w0, u32 w1) {
    u32 idx = w0 & 0xFF;
    u32 ofs = ((w0 >> 8) & 0xFF) * 8;
    const u8* d = pc_resolve_addr(w1);

    if (d == NULL) {
        return;
    }
    if (idx == G_MV_VIEWPORT) {
        const Vp* vp = (const Vp*)d;
        float sx = vp->vp.vscale[0] / 4.0f, sy = vp->vp.vscale[1] / 4.0f;
        float tx = vp->vp.vtrans[0] / 4.0f, ty = vp->vp.vtrans[1] / 4.0f;

        flush_for(FR_VIEWPORT);
        rsp.vpX = tx - sx;
        rsp.vpY = ty - sy;
        rsp.vpW = 2.0f * sx;
        rsp.vpH = 2.0f * sy;
    } else if (idx == G_MV_LIGHT) {
        if (ofs == G_MVO_LOOKATX || ofs == G_MVO_LOOKATY) {
            float* la = rsp.lookat[ofs == G_MVO_LOOKATY];
            la[0] = (s8)d[8];
            la[1] = (s8)d[9];
            la[2] = (s8)d[10];
            normalize3(la);
        } else {
            int i = (int)(ofs - G_MVO_L0) / 24;
            if (i >= 0 && i < rsp.numLights && i < MAX_LIGHTS) {
                RspLight* l = &rsp.lights[i];
                l->col[0] = d[0] / 255.0f;
                l->col[1] = d[1] / 255.0f;
                l->col[2] = d[2] / 255.0f;
                l->dir[0] = (s8)d[8];
                l->dir[1] = (s8)d[9];
                l->dir[2] = (s8)d[10];
                normalize3(l->dir);
            } else if (i == rsp.numLights) {
                rsp.ambient[0] = d[0] / 255.0f;
                rsp.ambient[1] = d[1] / 255.0f;
                rsp.ambient[2] = d[2] / 255.0f;
            }
        }
        rsp.lightsDirty = 1;
    }
}

static void handle_moveword(u32 w0, u32 w1) {
    u32 idx = (w0 >> 16) & 0xFF;
    u32 ofs = w0 & 0xFFFF;

    switch (idx) {
        case G_MW_SEGMENT:
            if ((ofs >> 2) < 16) {
                gSegments[ofs >> 2] = w1;
            }
            break;
        case G_MW_NUMLIGHT:
            rsp.numLights = w1 / 24;
            if (rsp.numLights > MAX_LIGHTS) {
                rsp.numLights = MAX_LIGHTS;
            }
            rsp.lightsDirty = 1;
            break;
        case G_MW_FOG:
            rsp.fogMul = (s16)(w1 >> 16);
            rsp.fogOfs = (s16)w1;
            break;
        case G_MW_LIGHTCOL: {
            int i = (int)(ofs / 24) - 2; // lights start after the two lookats
            if (i >= 0 && i < MAX_LIGHTS && (ofs % 24) == 0) {
                rsp.lights[i].col[0] = (w1 >> 24) / 255.0f;
                rsp.lights[i].col[1] = ((w1 >> 16) & 0xFF) / 255.0f;
                rsp.lights[i].col[2] = ((w1 >> 8) & 0xFF) / 255.0f;
            } else if (i == rsp.numLights) {
                rsp.ambient[0] = (w1 >> 24) / 255.0f;
                rsp.ambient[1] = ((w1 >> 16) & 0xFF) / 255.0f;
                rsp.ambient[2] = ((w1 >> 8) & 0xFF) / 255.0f;
            }
            break;
        }
    }
}

static void handle_modifyvtx(u32 w0, u32 w1) {
    u32 where = (w0 >> 16) & 0xFF;
    RspVtx* v = &rsp.vtx[((w0 & 0xFFFF) >> 1) % MAX_VERTS];

    if (where == G_MWO_POINT_RGBA) {
        v->r = (w1 >> 24) / 255.0f;
        v->g = ((w1 >> 16) & 0xFF) / 255.0f;
        v->b = ((w1 >> 8) & 0xFF) / 255.0f;
        v->a = (w1 & 0xFF) / 255.0f;
    } else if (where == G_MWO_POINT_ST) {
        v->u = (s16)(w1 >> 16);
        v->v = (s16)w1;
    }
}

/* ---------------------------------------------------------------------------
 * RDP rectangles
 * ------------------------------------------------------------------------- */

static float rect_depth(void) {
    if (rdp.otherL & G_ZS_PRIM) {
        return (rdp.primDepth / 32767.0f) * 2.0f - 1.0f;
    }
    return -1.0f;
}

/* Screen rect (N64 pixels) as two triangles, uv per corner */
/* Shade color of rectangles: white, except fill rectangles in 1/2-cycle
   mode, which have no shade data on the RDP (shade reads 0: MM's letterbox
   edge lines, prim alpha 0 through a leftover SHADE combiner, are invisible) */
static const float* sRectShade;

static void push_rect(float x0, float y0, float x1, float y1, float u0[2], float u1[2], float t0[2], float t1[2],
                      int flip) {
    static const float white[4] = { 1, 1, 1, 1 };
    const float* shade = sRectShade != NULL ? sRectShade : white;
    float z = rect_depth();
    float w = sAuxCur != NULL ? sAuxCur->width : S30_2D_W, h = sAuxCur != NULL ? AUX_H : SCREEN_H;
    float pad = sAuxCur != NULL ? 0.0f : WIDE_PAD; // 2D layer: N64 x + margin
    float X0 = (x0 + pad) / (w / 2.0f) - 1.0f, X1 = (x1 + pad) / (w / 2.0f) - 1.0f;
    float Y0 = 1.0f - y0 / (h / 2.0f), Y1 = 1.0f - y1 / (h / 2.0f);
    // corner uvs: (u at x0/x1, v at y0/y1), swapped axes when flipped
    float ua0 = u0[0], va0 = u0[1], ua1 = u1[0], va1 = u1[1];
    float ub0 = t0[0], vb0 = t0[1], ub1 = t1[0], vb1 = t1[1];

#define RV(X, Y, sx, sy)                                                                                 \
    push_vertex(X, Y, z, 1.0f, shade, flip ? ua0 + (ua1 - ua0) * (sy) : ua0 + (ua1 - ua0) * (sx),       \
                flip ? va0 + (va1 - va0) * (sx) : va0 + (va1 - va0) * (sy),                              \
                flip ? ub0 + (ub1 - ub0) * (sy) : ub0 + (ub1 - ub0) * (sx),                              \
                flip ? vb0 + (vb1 - vb0) * (sx) : vb0 + (vb1 - vb0) * (sy), 0.0f)
    RV(X0, Y0, 0, 0);
    RV(X1, Y0, 1, 0);
    RV(X0, Y1, 0, 1);
    RV(X1, Y0, 1, 0);
    RV(X1, Y1, 1, 1);
    RV(X0, Y1, 0, 1);
#undef RV
}

static void handle_fillrect(u32 w0, u32 w1) {
    float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f;
    float ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
    u32 cyc = rdp.otherH & (3 << G_MDSFT_CYCLETYPE);
    float zero[2] = { 0, 0 };

    if (rdp.cimg == rdp.zimg) {
        // Clearing the depth buffer (the game fills the z image)
        flush_for(FR_ZCLEAR);
        S30Display_Bind3D();
        gl_cache_reset();
        glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);
        return;
    }
    if (!drawing_enabled()) {
        return;
    }
    // Fill rectangles cover their lower-right pixel in every cycle mode
    (void)cyc;
    lrx += 1.0f;
    lry += 1.0f;
    if (sAuxCur == NULL && ulx <= 0.0f && lrx >= SCREEN_W) {
        // Full-width fill (fade, letterbox bar): to the panel's edges
        ulx = -WIDE_PAD;
        lrx = SCREEN_W + WIDE_PAD;
    }
    sStats.fills++;
    begin_batch(1);
    {
        static const float none[4] = { 0, 0, 0, 0 };

        sRectShade = none;
        push_rect(ulx, uly, lrx, lry, zero, zero, zero, zero, 0);
        sRectShade = NULL;
    }
}

/* <PREFIX>_HUDTRACE: the C-button area's rects of each frame, appended to
   hudtrace.log (cwd) whenever they differ from the frame before */
static int sHudTrace = -1;
static char sHudLine[2048], sHudPrev[2048];
static size_t sHudLen;

static void hud_trace_rect(float ulx, float uly, int tile) {
    const Tile* t = &rdp.tiles[tile & 7];
    int w, h;
    TexEntry* e;

    if (sHudTrace < 0) {
        sHudTrace = S30_Getenv("HUDTRACE") != NULL;
    }
    if (!sHudTrace || sAuxCur != NULL || ulx < 200.0f || uly >= 80.0f || sHudLen + 160 > sizeof(sHudLine)) {
        return;
    }
    e = tex_lookup(tile, &w, &h);
    sHudLen += snprintf(sHudLine + sHudLen, sizeof(sHudLine) - sHudLen,
                        " [%.0f,%.0f f%d/%d %dx%d t%d h%04x pa%.2f c%08x:%08x L%08x H%06x sc%d,%d,%d,%d]", ulx, uly,
                        t->fmt, t->siz, w, h, t->tmem, (unsigned)(e->hash & 0xFFFF), rdp.prim[3], rdp.combW0,
                        rdp.combW1, rdp.otherL, rdp.otherH, rdp.scX, rdp.scY, rdp.scW, rdp.scH);
}

static void hud_trace_frame(void) {
    static FILE* f;

    if (sHudTrace <= 0) {
        return;
    }
    if (strcmp(sHudLine, sHudPrev) != 0) {
        if (f == NULL) {
            f = fopen("hudtrace.log", "a");
        }
        if (f != NULL) {
            fprintf(f, "f%u:%s\n", sFrame, sHudLine);
            fflush(f);
        }
        memcpy(sHudPrev, sHudLine, sizeof(sHudLine));
    }
    sHudLen = 0;
    sHudLine[0] = 0;
}

static void handle_texrect(u32 w0, u32 w1, u32 h1, u32 h2, int flip) {
    float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f;
    int tile = (w1 >> 24) & 7;
    float ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
    float s = (s16)(h1 >> 16), t = (s16)(h1 & 0xFFFF); // 10.5
    float dsdx = (s16)(h2 >> 16) / 1024.0f, dtdy = (s16)(h2 & 0xFFFF) / 1024.0f;
    u32 cyc = rdp.otherH & (3 << G_MDSFT_CYCLETYPE);
    float w, h, s1, t1;
    float a0[2], a1[2], b0[2], b1[2];

    if (!drawing_enabled()) {
        return;
    }
    if (cyc == G_CYC_COPY) {
        dsdx /= 4.0f;
        lrx += 1.0f;
        lry += 1.0f;
    }
    w = lrx - ulx;
    h = lry - uly;
    // 10.5 texel coords at the far corners
    s1 = s + (flip ? h : w) * dsdx * 32.0f;
    t1 = t + (flip ? w : h) * dtdy * 32.0f;

    // Rects name their own tile; consecutive ones with the same tile and state
    // share a draw (state changes and texture loads flush as for triangles)
    sStats.rects++;
    begin_batch_tile(1, tile);
    tile_uv(tile, s, t, &a0[0], &a0[1]);
    tile_uv(tile, s1, t1, &a1[0], &a1[1]);
    if (sTraceFrame != 0 && sFrame == sTraceFrame) {
        Tile* tl = &rdp.tiles[tile];
        printf("rect: %.1f,%.1f-%.1f,%.1f tile %d s %.1f t %.1f dsdx %.3f dtdy %.3f uls %d ult %d lrs %d lrt %d "
               "uv %.3f,%.3f-%.3f,%.3f cyc %u\n",
               ulx, uly, lrx, lry, tile, s / 32, t / 32, dsdx, dtdy, tl->uls / 4, tl->ult / 4, tl->lrs / 4,
               tl->lrt / 4, a0[0], a0[1], a1[0], a1[1], cyc);
    }
    tile_uv(tile + 1, s, t, &b0[0], &b0[1]);
    tile_uv(tile + 1, s1, t1, &b1[0], &b1[1]);
    push_rect(ulx, uly, lrx, lry, a0, a1, b0, b1, flip);
    hud_trace_rect(ulx, uly, tile);
}

/* S2DEX G_BG_COPY / G_BG_1CYC: the prerendered room background. The game
   decodes the room JPEG in place (Room_DecodeJpeg) to native-endian RGBA5551,
   so the image is uploaded as is rather than through TMEM, and drawn as one
   screen rect. It stays cached until the image changes. */
static void handle_bg(u32 w1, int copy) {
    static GLuint tex;
    static const void* texSrc;
    static u64 texHash;
    static int texW, texH;
    const uObjBg* bg = pc_resolve_addr(w1);
    const u16* px;
    int w, h, i;
    u64 hash = 0xCBF29CE484222325ull;
    float sx = 1.0f, sy = 1.0f;
    float x0, y0, x1, y1, a0[2], a1[2], zero[2] = { 0, 0 };

    if (bg == NULL || !drawing_enabled() || sAuxCur != NULL) {
        return;
    }
    w = bg->b.imageW >> 2;
    h = bg->b.imageH >> 2;
    px = pc_resolve_addr((u32)(uintptr_t)bg->b.imagePtr);
    if (px == NULL || w <= 0 || h <= 0) {
        return;
    }
    if (bg->b.imageFmt != G_IM_FMT_RGBA || bg->b.imageSiz != G_IM_SIZ_16b) {
        static int warned;
        if (!warned++) {
            printf("gfx: S2DEX background fmt %d siz %d not supported\n", bg->b.imageFmt, bg->b.imageSiz);
        }
        return;
    }
    // Sampled content hash: the same buffer is reused for each room's image
    for (i = 0; i < 256; i++) {
        hash = (hash ^ px[(u32)i * (w * h / 256)]) * 0x100000001B3ull;
    }
    flush_for(FR_BG);
    if (tex == 0 || texSrc != px || texHash != hash || texW != w || texH != h) {
        if (tex == 0) {
            glGenTextures(1, &tex);
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        sGL.tex[0] = tex;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, px);
        sStats.uploads++;
        texSrc = px;
        texHash = hash;
        texW = w;
        texH = h;
    }
    if (!copy) {
        const uObjScaleBg_t* sbg = &bg->s;
        sx = sbg->scaleW / 1024.0f;
        sy = sbg->scaleH / 1024.0f;
    }
    x0 = bg->b.frameX / 4.0f;
    y0 = bg->b.frameY / 4.0f;
    x1 = x0 + bg->b.frameW / 4.0f;
    y1 = y0 + bg->b.frameH / 4.0f;
    a0[0] = bg->b.imageX / 32.0f / w;
    a0[1] = bg->b.imageY / 32.0f / h;
    a1[0] = a0[0] + (x1 - x0) / sx / w;
    a1[1] = a0[1] + (y1 - y0) / sy / h;
    if (x1 - x0 >= SCREEN_W - 8 && y1 - y0 >= SCREEN_H - 8) {
        // A full-screen background moved by a screen shake (frameX/Y offset):
        // cover the whole screen, its edge pixels repeated into the gap
        // (texture clamp), rather than showing the room's depth-only geometry
        float du = 1.0f / (sx * w), dv = 1.0f / (sy * h);
        if (x0 > 0.0f) {
            a0[0] -= x0 * du;
            x0 = 0.0f;
        }
        if (y0 > 0.0f) {
            a0[1] -= y0 * dv;
            y0 = 0.0f;
        }
        if (x1 < SCREEN_W) {
            a1[0] += (SCREEN_W - x1) * du;
            x1 = SCREEN_W;
        }
        if (y1 < SCREEN_H) {
            a1[1] += (SCREEN_H - y1) * dv;
            y1 = SCREEN_H;
        }
    }
    if (bg->b.imageFlip & G_BG_FLAG_FLIPS) {
        float t = a0[0];
        a0[0] = a1[0];
        a1[0] = t;
    }
    sStats.rects++;
    sBgTex = tex;
    sHadBg = 1;
    begin_batch(1);
    push_rect(x0, y0, x1, y1, a0, a1, zero, zero, 0);
    flush_for(FR_BG);
    sBgTex = 0;
}

/* ---------------------------------------------------------------------------
 * Display list walk
 * ------------------------------------------------------------------------- */

static void color_from_word(float c[4], u32 w) {
    c[0] = (w >> 24) / 255.0f;
    c[1] = ((w >> 16) & 0xFF) / 255.0f;
    c[2] = ((w >> 8) & 0xFF) / 255.0f;
    c[3] = (w & 0xFF) / 255.0f;
}

/* Mode bits the renderer actually uses: other changes need no flush */
#define OTHERH_USED ((3u << G_MDSFT_CYCLETYPE) | (3u << G_MDSFT_TEXTFILT) | (3u << G_MDSFT_TEXTLUT))
#define OTHERL_USED (0xFFFF0000u | 0x7C3Fu)
#define GEOM_USED (G_ZBUFFER | G_CULL_BOTH | G_FOG | G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR)

static void set_othermode(u32* mode, u32 w0, u32 w1) {
    u32 len = (w0 & 0xFF) + 1;
    u32 shift = 32 - ((w0 >> 8) & 0xFF) - len;
    u32 mask = (len >= 32 ? 0xFFFFFFFF : ((1u << len) - 1)) << shift;

    *mode = (*mode & ~mask) | (w1 & mask);
}

/* ---------------------------------------------------------------------------
 * F3DEX (Star Fox 64): each RSP command re-encoded as its F3DEX2 equivalent
 * (the RDP commands are the same). Opcodes, matrix flags, geometry mode bits,
 * memory/word move indices and the light encodings differ.
 * ------------------------------------------------------------------------- */

enum {
    F3DEX_SPNOOP = 0x00,
    F3DEX_MTX = 0x01,
    F3DEX_MOVEMEM = 0x03,
    F3DEX_VTX = 0x04,
    F3DEX_DL = 0x06,
    F3DEX_LOAD_UCODE = 0xAF,
    F3DEX_BRANCH_Z = 0xB0,
    F3DEX_TRI2 = 0xB1,
    F3DEX_MODIFYVTX = 0xB2,
    F3DEX_RDPHALF_2 = 0xB3,
    F3DEX_RDPHALF_1 = 0xB4,
    F3DEX_LINE3D = 0xB5,
    F3DEX_CLEARGEOMETRYMODE = 0xB6,
    F3DEX_SETGEOMETRYMODE = 0xB7,
    F3DEX_ENDDL = 0xB8,
    F3DEX_SETOTHERMODE_L = 0xB9,
    F3DEX_SETOTHERMODE_H = 0xBA,
    F3DEX_TEXTURE = 0xBB,
    F3DEX_MOVEWORD = 0xBC,
    F3DEX_POPMTX = 0xBD,
    F3DEX_CULLDL = 0xBE,
    F3DEX_TRI1 = 0xBF,
};
#define F3DEX_MV_VIEWPORT 0x80
#define F3DEX_MV_LOOKATY 0x82
#define F3DEX_MV_LOOKATX 0x84
#define F3DEX_MV_L0 0x86
#define F3DEX_MTX_PROJECTION 0x01
#define F3DEX_MTX_LOAD 0x02
#define F3DEX_MTX_PUSH 0x04

/* F3DEX geometry mode bits as F3DEX2's: culling and smooth shading moved */
static u32 f3dex_geom(u32 g) {
    u32 r = g & ~(0x1000u | 0x2000u | 0x200u);

    r |= (g & 0x1000u) ? G_CULL_FRONT : 0;
    r |= (g & 0x2000u) ? G_CULL_BACK : 0;
    r |= (g & 0x200u) ? G_SHADING_SMOOTH : 0;
    return r;
}

/* Rewrites an F3DEX command in place as F3DEX2. Returns 0 to skip it. */
static int f3dex_to_f3dex2(u32* pw0, u32* pw1) {
    u32 w0 = *pw0, w1 = *pw1;
    u8 cmd = w0 >> 24;

    if (cmd >= 0xC0) {
        return 1; // RDP: the same
    }
    switch (cmd) {
        case F3DEX_SPNOOP:
            return 0;
        case F3DEX_VTX: { // (n << 10) | (n * 16 - 1), v0 * 2 in bits 16-23
            u32 n = (w0 >> 10) & 0x3F, v0 = ((w0 >> 16) & 0xFF) / 2;
            w0 = ((u32)G_VTX << 24) | (n << 12) | ((v0 + n) << 1);
            break;
        }
        case F3DEX_TRI1: // indices * 2 in w1
            w0 = ((u32)G_TRI1 << 24) | (w1 & 0xFFFFFF);
            w1 = 0;
            break;
        case F3DEX_TRI2:
            w0 = ((u32)G_TRI2 << 24) | (w0 & 0xFFFFFF);
            break;
        case F3DEX_MTX: { // flags in bits 16-23; F3DEX2 inverts the push bit
            u32 p = (w0 >> 16) & 0xFF;
            w0 = ((u32)G_MTX << 24) | ((p & F3DEX_MTX_PROJECTION) ? G_MTX_PROJECTION : 0) |
                 ((p & F3DEX_MTX_LOAD) ? G_MTX_LOAD : 0) | ((p & F3DEX_MTX_PUSH) ? 0 : G_MTX_PUSH);
            break;
        }
        case F3DEX_POPMTX:
            w0 = (u32)G_POPMTX << 24;
            w1 = 64; // one matrix
            break;
        case F3DEX_SETGEOMETRYMODE:
            w0 = ((u32)G_GEOMETRYMODE << 24) | 0xFFFFFF;
            w1 = f3dex_geom(w1);
            break;
        case F3DEX_CLEARGEOMETRYMODE:
            w0 = ((u32)G_GEOMETRYMODE << 24) | (~f3dex_geom(w1) & 0xFFFFFF);
            w1 = 0;
            break;
        case F3DEX_TEXTURE: // on in bits 0-7 (F3DEX2: bits 1-7)
            w0 = ((u32)G_TEXTURE << 24) | (w0 & 0x3F00) | ((w0 & 0x7F) << 1);
            break;
        case F3DEX_SETOTHERMODE_L:
        case F3DEX_SETOTHERMODE_H: { // shift and length as they are
            u32 sft = (w0 >> 8) & 0xFF, len = w0 & 0xFF;
            w0 = ((u32)(cmd == F3DEX_SETOTHERMODE_L ? G_SETOTHERMODE_L : G_SETOTHERMODE_H) << 24) |
                 (((32 - sft - len) & 0xFF) << 8) | ((len - 1) & 0xFF);
            break;
        }
        case F3DEX_MOVEWORD: { // offset in bits 8-23, index in 0-7
            u32 idx = w0 & 0xFF, ofs = (w0 >> 8) & 0xFFFF;
            if (idx == G_MW_NUMLIGHT) {
                s32 n = (s32)((w1 & 0x7FFFFFFF) / 32) - 1; // NUML(n) = (n + 1) * 32 + 0x80000000
                w1 = (u32)(n < 0 ? 0 : n) * 24;
            } else if (idx == G_MW_LIGHTCOL) {
                ofs = (ofs / 0x20 + 2) * 24 + (ofs % 0x20); // 0x20 per light; F3DEX2: after the two lookats
            }
            w0 = ((u32)G_MOVEWORD << 24) | (idx << 16) | (ofs & 0xFFFF);
            break;
        }
        case F3DEX_MOVEMEM: { // index in bits 16-23
            u32 idx = (w0 >> 16) & 0xFF, ofs;
            if (idx == F3DEX_MV_VIEWPORT) {
                w0 = ((u32)G_MOVEMEM << 24) | G_MV_VIEWPORT;
                break;
            }
            if (idx == F3DEX_MV_LOOKATX) {
                ofs = 0;
            } else if (idx == F3DEX_MV_LOOKATY) {
                ofs = 24;
            } else if (idx >= F3DEX_MV_L0 && idx <= F3DEX_MV_L0 + 14) {
                ofs = 48 + (idx - F3DEX_MV_L0) / 2 * 24;
            } else {
                return 0;
            }
            w0 = ((u32)G_MOVEMEM << 24) | ((ofs / 8) << 8) | G_MV_LIGHT;
            break;
        }
        case F3DEX_MODIFYVTX:
            w0 = ((u32)G_MODIFYVTX << 24) | (w0 & 0xFFFFFF);
            break;
        case F3DEX_CULLDL:
            if (sCfg.ucode == S30_UCODE_F3DEX_OLD) { // vertex index * 40, end + 1
                u32 v0 = (w0 & 0xFFFF) / 40, v1 = (w1 & 0xFFFF) / 40;
                w0 = ((u32)G_CULLDL << 24) | (v0 * 2);
                w1 = (v1 > 0 ? v1 - 1 : 0) * 2;
                break;
            }
            w0 = ((u32)G_CULLDL << 24) | (w0 & 0xFFFFFF);
            break;
        case F3DEX_BRANCH_Z:
            w0 = ((u32)G_BRANCH_Z << 24) | (w0 & 0xFFFFFF);
            break;
        case F3DEX_DL:
            w0 = ((u32)G_DL << 24) | (w0 & 0xFFFFFF);
            break;
        case F3DEX_ENDDL:
            w0 = (u32)G_ENDDL << 24;
            break;
        case F3DEX_RDPHALF_1:
            w0 = (u32)G_RDPHALF_1 << 24;
            break;
        case F3DEX_RDPHALF_2:
            w0 = (u32)G_RDPHALF_2 << 24;
            break;
        case F3DEX_LINE3D:
            if (sCfg.ucode == S30_UCODE_F3DEX_OLD) {
                // G_QUAD: v3, v0, v1, v2 (* 2) in w1, as triangles v0 v1 v2
                // and v0 v2 v3
                u32 a = (w1 >> 16) & 0xFF, b = (w1 >> 8) & 0xFF, c = w1 & 0xFF, d = w1 >> 24;
                w0 = ((u32)G_TRI2 << 24) | (a << 16) | (b << 8) | c;
                w1 = (a << 16) | (c << 8) | d;
                break;
            }
            return 0;
        default: // LOAD_UCODE, unknown
            return 0;
    }
    *pw0 = w0;
    *pw1 = w1;
    return 1;
}

static void walk(Gfx* dl, int depth) {
    if (dl == NULL || depth > DL_DEPTH || ((uintptr_t)dl & 3)) {
        return;
    }
    for (;; dl++) {
        u32 w0 = dl->words.w0, w1 = dl->words.w1;
        u8 cmd;

        if (sCfg.ucode != S30_UCODE_F3DEX2 && !f3dex_to_f3dex2(&w0, &w1)) {
            continue;
        }
        cmd = w0 >> 24;

        sStats.cmds++;
        if (w0 == S30_GFX_MARK) {
            flush_for(FR_MARK);
            if (w1 == S30_MARK_BG_SAVE) {
                S30Display_SaveBackground();
            } else if (w1 == S30_MARK_BG_RESTORE) {
                S30Display_RestoreBackground();
            } else if (w1 == S30_MARK_VISMONO) {
                S30Display_VisMono(rdp.prim, rdp.env);
            } else if (w1 == S30_MARK_MBLUR_BLEND) {
                S30Display_MotionBlurBlend(rdp.prim[3]);
            } else if (w1 == S30_MARK_MBLUR_SAVE) {
                S30Display_MotionBlurSave();
            } else if (w1 == S30_MARK_FB_READ) {
                read_frame();
            }
            gl_cache_reset();
            continue;
        }
        switch (cmd) {
            case G_VTX:
                handle_vtx(w0, w1);
                break;
            case G_MODIFYVTX:
                handle_modifyvtx(w0, w1);
                break;
            case G_TRI1:
                tri(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                break;
            case G_TRI2:
            case G_QUAD:
                tri(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                tri(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
                break;
            case G_MTX:
                handle_mtx(w0, w1);
                break;
            case G_POPMTX: {
                u32 n = w1 / 64;
                rsp.mvTop = (u32)rsp.mvTop >= n ? rsp.mvTop - (int)n : 0;
                rsp.mvpDirty = 1;
                rsp.lightsDirty = 1;
                break;
            }
            case G_GEOMETRYMODE: {
                u32 g = (rsp.geom & w0) | w1;
                if (g != rsp.geom) {
                    // Lighting/fog/texgen are applied per vertex at G_VTX and
                    // culling per triangle; only the z-buffer is draw state
                    if ((g ^ rsp.geom) & G_ZBUFFER) {
                        flush_for(FR_GEOM);
                    }
                    if ((g ^ rsp.geom) & (G_LIGHTING | G_TEXTURE_GEN)) {
                        rsp.lightsDirty = 1;
                    }
                    rsp.geom = g;
                }
                break;
            }
            case G_MOVEMEM:
                handle_movemem(w0, w1);
                break;
            case G_MOVEWORD:
                handle_moveword(w0, w1);
                break;
            case G_TEXTURE: {
                int on = (w0 >> 1) & 0x7F;
                int tile = (w0 >> 8) & 7;
                if (on != rsp.texOn || tile != rsp.texTile) {
                    flush_for(FR_TEXTURE);
                }
                rsp.texScaleS = w1 >> 16;
                rsp.texScaleT = w1 & 0xFFFF;
                rsp.texOn = on;
                rsp.texTile = tile;
                break;
            }
            case G_DL:
                sStats.dls++;
                if (sTraceFrame != 0 && sFrame == sTraceFrame) {
                    printf("%*sdl %08x -> %p\n", depth * 2, "", (unsigned)w1, pc_resolve_addr(w1));
                }
                walk(pc_resolve_addr(w1), depth + 1);
                if ((w0 >> 16) & 0xFF) {
                    return; // branch, no return
                }
                break;
            case G_BRANCH_Z:
                walk(pc_resolve_addr(rsp.rdpHalf1), depth + 1);
                return;
            case G_ENDDL:
                return;
            case G_CULLDL: {
                u32 v0 = (w0 & 0xFFFF) / 2, v1 = (w1 & 0xFFFF) / 2, i;
                u8 c = 0xFF;
                for (i = v0; i <= v1 && i < MAX_VERTS; i++) {
                    c &= rsp.vtx[i].clip;
                }
                if (v1 < MAX_VERTS && (c & ~CLIP_NEAR)) {
                    return; // all outside the same side plane
                }
                break;
            }
            case G_RDPHALF_1:
                rsp.rdpHalf1 = w1;
                break;
            case G_RDPHALF_2:
                rsp.rdpHalf2 = w1;
                break;
            case G_TEXRECT:
            case G_TEXRECTFLIP: {
                // Followed by RDPHALF_1 (s, t) and RDPHALF_2 (dsdx, dtdy)
                u32 h1 = dl[1].words.w1, h2 = dl[2].words.w1;
                handle_texrect(w0, w1, h1, h2, cmd == G_TEXRECTFLIP);
                dl += 2;
                break;
            }
            case G_FILLRECT:
                handle_fillrect(w0, w1);
                break;
            case G_SETCIMG: {
                u32 width = (w0 & 0xFFF) + 1;

                sStats.cimgs++;
                flush_for(FR_CIMG);
                rdp.cimg = w1;
                rdp.cimgIsFrame = width == SCREEN_W;
                sAuxCur = NULL;
                if (!rdp.cimgIsFrame && ((w0 >> 19) & 3) == G_IM_SIZ_16b && width <= AUX_MAX_W) {
                    sAuxCur = aux_get(pc_resolve_addr(w1), width);
                }
                break;
            }
            case G_SETZIMG:
                rdp.zimg = w1;
                if (sAuxCur != NULL) {
                    // Rendering to an auxiliary image with its own z-buffer
                    // (cleared by the game just before)
                    flush_for(FR_ZIMG);
                    aux_bind(sAuxCur);
                    gl_cache_reset();
                    glDisable(GL_SCISSOR_TEST);
                    glDepthMask(GL_TRUE);
                    glClear(GL_DEPTH_BUFFER_BIT);
                }
                break;
            case G_SETTIMG:
                rdp.timgFmt = (w0 >> 21) & 7;
                rdp.timgSiz = (w0 >> 19) & 3;
                rdp.timgWidth = (w0 & 0xFFF) + 1;
                rdp.timgAddr = w1;
                sTimgCmd = dl;
                break;
            case G_SETCOMBINE: {
                // 1-cycle draws read only the first cycle's terms: a change in
                // the second alone needs no new draw (a switch to 2-cycle
                // flushes as a mode change)
                int one = (rdp.otherH & (3 << G_MDSFT_CYCLETYPE)) == G_CYC_1CYCLE;
                u32 m0 = one ? COMB1_W0 : 0xFFFFFF, m1 = one ? COMB1_W1 : 0xFFFFFFFF;

                if (((w0 ^ rdp.combW0) & m0) || ((w1 ^ rdp.combW1) & m1)) {
                    flush_for(FR_COMBINE);
                }
                rdp.combW0 = w0 & 0xFFFFFF;
                rdp.combW1 = w1;
                break;
            }
            case G_SETOTHERMODE_H: {
                u32 m = rdp.otherH;
                set_othermode(&m, w0, w1);
                if ((m ^ rdp.otherH) & OTHERH_USED) {
                    flush_for(FR_OMODEH);
                }
                rdp.otherH = m;
                break;
            }
            case G_SETOTHERMODE_L: {
                u32 m = rdp.otherL;
                set_othermode(&m, w0, w1);
                if ((m ^ rdp.otherL) & OTHERL_USED) {
                    flush_for(FR_OMODEL);
                }
                rdp.otherL = m;
                break;
            }
            case G_RDPSETOTHERMODE:
                if (((w0 & 0xFFFFFF) ^ rdp.otherH) & OTHERH_USED || (w1 ^ rdp.otherL) & OTHERL_USED) {
                    flush_for(FR_RDPOMODE);
                }
                rdp.otherH = w0 & 0xFFFFFF;
                rdp.otherL = w1;
                break;
            case G_SETPRIMCOLOR:
                // per-vertex attribute: no flush needed
                color_from_word(rdp.prim, w1);
                if (rdp.primLod != (w0 & 0xFF) / 255.0f) {
                    flush_for(FR_PRIMLOD);
                    rdp.primLod = (w0 & 0xFF) / 255.0f;
                }
                break;
            case G_SETENVCOLOR:
                color_from_word(rdp.env, w1);
                break;
            case G_SETFOGCOLOR: {
                float c[4];
                color_from_word(c, w1);
                if (memcmp(c, rdp.fog, sizeof(c)) != 0) {
                    flush_for(FR_FOGCOL);
                    memcpy(rdp.fog, c, sizeof(c));
                }
                break;
            }
            case G_SETBLENDCOLOR: {
                float c[4];
                color_from_word(c, w1);
                if (memcmp(c, rdp.blend, sizeof(c)) != 0) {
                    flush_for(FR_BLENDCOL);
                    memcpy(rdp.blend, c, sizeof(c));
                }
                break;
            }
            case G_SETFILLCOLOR: {
                u16 c = w1 & 0xFFFF;
                u8 r = (c >> 11) & 31, g = (c >> 6) & 31, b = (c >> 1) & 31;
                float f[4] = { r / 31.0f, g / 31.0f, b / 31.0f, (c & 1) ? 1.0f : 0.0f };
                if (memcmp(f, rdp.fill, sizeof(f)) != 0) {
                    flush_for(FR_FILLCOL);
                    memcpy(rdp.fill, f, sizeof(f));
                }
                break;
            }
            case G_SETPRIMDEPTH:
                rdp.primDepth = w1 >> 16;
                break;
            case G_SETSCISSOR: {
                int x = ((w0 >> 12) & 0xFFF) / 4, y = (w0 & 0xFFF) / 4;
                int w = ((w1 >> 12) & 0xFFF) / 4 - x, h = (w1 & 0xFFF) / 4 - y;
                if (x != rdp.scX || y != rdp.scY || w != rdp.scW || h != rdp.scH) {
                    flush_for(FR_SCISSOR);
                    rdp.scX = x;
                    rdp.scY = y;
                    rdp.scW = w;
                    rdp.scH = h;
                }
                break;
            }
            case G_SETTILE: {
                int ti = (w1 >> 24) & 7;
                Tile n = rdp.tiles[ti];
                n.fmt = (w0 >> 21) & 7;
                n.siz = (w0 >> 19) & 3;
                n.line = (w0 >> 9) & 0x1FF;
                n.tmem = w0 & 0x1FF;
                n.palette = (w1 >> 20) & 0xF;
                n.cmt = (w1 >> 18) & 3;
                n.maskt = (w1 >> 14) & 0xF;
                n.shiftt = (w1 >> 10) & 0xF;
                n.cms = (w1 >> 8) & 3;
                n.masks = (w1 >> 4) & 0xF;
                n.shifts = w1 & 0xF;
                set_tile(ti, &n);
                break;
            }
            case G_SETTILESIZE: {
                int ti = (w1 >> 24) & 7;
                Tile n = rdp.tiles[ti];
                n.uls = (w0 >> 12) & 0xFFF;
                n.ult = w0 & 0xFFF;
                n.lrs = (w1 >> 12) & 0xFFF;
                n.lrt = w1 & 0xFFF;
                set_tile(ti, &n);
                break;
            }
            case G_LOADBLOCK:
                load_block((w1 >> 24) & 7, (w0 >> 12) & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF);
                break;
            case G_LOADTILE:
                load_tile((w1 >> 24) & 7, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF);
                break;
            case G_BG_COPY:
            case G_BG_1CYC:
                handle_bg(w1, cmd == G_BG_COPY);
                break;
            case G_LOADTLUT:
                load_tlut((w1 >> 24) & 7, ((w1 >> 14) & 0x3FF) + 1);
                break;
            default:
                break;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Entry points
 * ------------------------------------------------------------------------- */

void S30Gfx_Init(void) {
    const char* ext = (const char*)glGetString(GL_EXTENSIONS);

    sHasNpot = ext != NULL && strstr(ext, "GL_OES_texture_npot") != NULL;
    printf("gfx: npot textures %s\n", sHasNpot ? "yes" : "no");

    // The combiner's NOISE input: 64x64 random intensities on texture unit 2,
    // which nothing else binds
    {
        static u8 noise[64 * 64];
        GLuint tex;
        u32 r = 0x4E4F4953; // "NOIS"
        int i;

        for (i = 0; i < 64 * 64; i++) {
            r = r * 1103515245u + 12345u;
            noise[i] = r >> 24;
        }
        glGenTextures(1, &tex);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 64, 64, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, noise);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glActiveTexture(GL_TEXTURE0);
    }
}

/* The game edited a texture in place: forget load memos so the next load
   copies (and content-hashes) it again */
void gfx_texture_cache_invalidate(void* addr) {
    (void)addr;
    sLoadGen++;
}

void flush_texture_cache_external(void) {
    sLoadGen++;
}

/* Graph callback depth tests, late (the N64 reads the last frame's z-buffer;
   here two frames back, so the readback doesn't wait on the GPU: see
   S30Display_ReadProbes): call i of a frame returns the result of call i of
   an earlier frame and queues this one */
static unsigned char sDepthResults[32];
static int sNumDepthResults, sDepthCall;

void S30Gfx_BeginDepthTests(void) {
    sNumDepthResults = S30Display_ReadProbes(sDepthResults, (int)sizeof(sDepthResults));
    sDepthCall = 0;
    gl_cache_reset();
}

int S30Gfx_DepthVisible(float x, float y, float z) {
    int i = sDepthCall++;

    if (x < -WIDE_PAD || y < 0.0f || x >= SCREEN_W + WIDE_PAD || y >= SCREEN_H) {
        return 0;
    }
    // Centre of the N64 pixel; GL rows run bottom-up
    S30Display_QueueProbe(i, (int)((x + WIDE_PAD + 0.5f) * FB_SCALE), (int)((SCREEN_H - y - 0.5f) * FB_SCALE), depth_ndc(z));
    return i < sNumDepthResults ? sDepthResults[i] : 1;
}


/* Frame time split for <PREFIX>_GFXSTATS: game logic (between frames), display
   list walk + GL calls (CPU), GPU finish, present */
static double sTGame, sTWalk, sTGpu, sTPresent, sTLast, sTMax;

void pc_process_displaylist(void* displayList) {
    Gfx* dl = displayList;
    int i;
    double t0 = now_ms(), t1, t2, t3;

    if (sTLast != 0) {
        sTGame += t0 - sTLast;
    }

    frame_copies(); // the last frame, sWide still its
    S30Display_BeginFrame();
    sFrame++;
    sWide = !sHadBg;
    sHadBg = 0;
    memset(&rsp.mv[0], 0, sizeof(rsp.mv[0]));
    for (i = 0; i < 4; i++) {
        rsp.mv[0][i][i] = 1.0f;
        rsp.p[i][i] = 1.0f;
    }
    rsp.mvTop = 0;
    // The microcode's default lookat (each task starts from its data): a
    // game that never sends gSPLookAt (Mario Kart 64's reflective logo)
    // texgens against X (1, 0, 0) and Y (0, 1, 0)
    memset(rsp.lookat, 0, sizeof(rsp.lookat));
    rsp.lookat[0][0] = 1.0f;
    rsp.lookat[1][1] = 1.0f;
    rsp.mvpDirty = 1;
    rsp.lightsDirty = 1;
    rsp.vpX = 0;
    rsp.vpY = 0;
    rsp.vpW = SCREEN_W;
    rsp.vpH = SCREEN_H;
    rdp.scX = 0;
    rdp.scY = 0;
    rdp.scW = SCREEN_W;
    rdp.scH = SCREEN_H;
    sLoadGen++;
    rdp.cimg = 0;
    rdp.cimgIsFrame = 1;
    sAuxCur = NULL;
    sCurShader = NULL;
    sBatchVerts = 0;
    // This frame's buffer in the ring (the attribute pointers follow it:
    // gl_cache_reset below)
    sVboFrame = (sVboFrame + 1) % VBO_RING;
    sVbo = sVboRing[sVboFrame];
    sVboNext = 0;
    gl_cache_reset();
    glBlendColor(0, 0, 0, 1);
    glPolygonOffset(-1.0f, -2.0f);
    glDepthFunc(GL_LEQUAL);
    glFrontFace(GL_CCW);

    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    walk(dl, 0);
    flush_for(FR_FRAME);
    t1 = now_ms();
    if (sStatsOn > 1) {
        glFinish(); // GPU time measured, at the cost of the CPU/GPU overlap
    }
    t2 = now_ms();
    if (sStatsOn < 0) {
        // <PREFIX>_GFXSTATS=1: stats every 100 frames; 2: also GPU time (glFinish)
        sStatsOn = S30_Getenv("GFXSTATS") != NULL ? (atoi(S30_Getenv("GFXSTATS")) > 1 ? 2 : 1) : 0;
        sTraceFrame = S30_Getenv("GFXTRACE") ? atoi(S30_Getenv("GFXTRACE")) : 0;
        signal(SIGUSR2, on_sigusr2);
    }
    if (sTraceRequest) {
        sTraceRequest = 0;
        sTraceFrame = sFrame + 1;
        printf("gfx: tracing frame %u\n", sTraceFrame);
    }
    if (sStatsOn && sFrame % 100 == 0) {
        printf("drawcall %.1f ms ", sTDraw / 100);
        sTDraw = 0;
        printf("time/frame ms: game %.1f walk %.1f (flush %.1f tex %.1f) gpu %.1f present %.1f | worst game+walk+gpu %.1f\n",
               sTGame / 100, sTWalk / 100, sTFlush / 100, sTTex / 100, sTGpu / 100, sTPresent / 100, sTMax);
        sTGame = sTWalk = sTGpu = sTPresent = sTFlush = sTTex = sTMax = 0;
        {
            int late, lost;
            S30Display_TakeLateFrames(&late, &lost);
            int r;
            printf("gfx: draws by cause, last 100 frames:");
            for (r = 0; r < FR_COUNT; r++) {
                if (sFlushWhy[r] != 0) {
                    printf(" %s %u", kFlushNames[r], (unsigned)sFlushWhy[r]);
                }
            }
            printf("\n");
            memset(sFlushWhy, 0, sizeof(sFlushWhy));
            printf("gfx[%u]: tris %u (clipped %u) draws %u rects %u fills %u uploads %u 2d->3d %u shaders %d "
                   "| late %d/100 (lost %d vblanks)\n",
                   sFrame, sStats.tris, sStats.trisClipped, sStats.draws, sStats.rects, sStats.fills, sStats.uploads,
                   sStats.switches, sNumShaders, late, lost);
        }
    }
    memset(&sStats, 0, sizeof(sStats));
    prof_tick(sFrame);
    hud_trace_frame();
    S30Display_EndFrame();
    t3 = now_ms();
    sTWalk += t1 - t0;
    if ((t2 - t0) + (sTLast != 0 ? t0 - sTLast : 0) > sTMax) {
        sTMax = (t2 - t0) + (sTLast != 0 ? t0 - sTLast : 0);
    }
    sTGpu += t2 - t1;
    sTPresent += t3 - t2;
    sTLast = t3;
}
