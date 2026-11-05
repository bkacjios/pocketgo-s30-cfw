#ifndef S30_GFX_H
#define S30_GFX_H

// N64 display list renderer for the PocketGo S30 (n64/src/gfx.c): the RSP
// microcode (F3DEX2, or F3DEX translated to it) and the RDP, drawn with GLES2
// into the display's framebuffer (s30_display.h).

#include <stdint.h>

typedef enum {
    S30_UCODE_F3DEX2, // Ocarina of Time, Majora's Mask (with S2DEX backgrounds)
    S30_UCODE_F3DEX,  // Star Fox 64
    // F3DEX 0.95 (Mario Kart 64): F3DEX with G_QUAD where LINE3D is, the old
    // G_CULLDL vertex encoding, and texrects ending in RDPHALF_CONT (0xB2)
    S30_UCODE_F3DEX_OLD,
} S30Ucode;

typedef struct {
    S30Ucode ucode;
    // The game's segment table (16 entries), read and written by the display
    // lists (G_MW_SEGMENT); a segmented address is table[seg] + offset
    uint32_t* segments;
    // Mtx as the game stores it: 0 for the N64's s15.16 fixed point (integer
    // halves, then fraction halves, each where the union names it), 1 for a
    // float[4][4] (ports that build their matrices in floats)
    int floatMtx;
    // Texture data byte order: the size in bytes of the elements the texture
    // at addr is stored as (1, 2, 4 or 8; little-endian elements, so the N64
    // byte at A is at A ^ (size - 1)). NULL: everything is u64s (A ^ 7).
    int (*texElemSize)(uintptr_t addr);
    // Debug environment variable prefix: <prefix>_GFXSTATS, _GFXTRACE, ...
    const char* envPrefix;
} S30GfxConfig;

// Call once, after S30Display_Init (the config is copied)
void S30Gfx_Configure(const S30GfxConfig* config);
void S30Gfx_Init(void);

// Draws a frame's display list and presents it (S30Display_EndFrame)
void pc_process_displaylist(void* displayList);

// Segmented address (top byte 0x00-0x0F, through the segment table) or a real
// pointer to memory
void* pc_resolve_addr(uint32_t addr);

// The game edited a texture in place / all textures: reload them
void gfx_texture_cache_invalidate(void* addr);
void flush_texture_cache_external(void);

// Replacement texture: a texture loaded from `src` draws `pixels` (w x h, both
// powers of 2; RGBA8888, or RGB565 if `rgb565`) instead, at their own
// resolution. `pixels` is malloc'd and handed over: freed once on the GPU.
void S30Gfx_TextureOverride(const void* src, int w, int h, int rgb565, void* pixels);

// A texture loaded from `dst` draws the N64 screen rectangle (x, y, w, h) of
// the frame shown last instead (copied on the GPU at the start of the next
// frame): a game copying its framebuffer into a texture (Mario Kart 64's
// jumbotron). Call again for each new copy.
void S30Gfx_FrameCopy(const void* dst, int x, int y, int w, int h);

// Depth tests against the z-buffer (sun, lens flare): whether a point at N64
// screen pixel (x, y) and NDC depth z passes. Results lag one frame (call i of
// a frame answers with call i of an earlier frame); S30Gfx_BeginDepthTests
// starts each frame's calls.
void S30Gfx_BeginDepthTests(void);
int S30Gfx_DepthVisible(float x, float y, float z);

// Display list markers: a command whose w0 is S30_GFX_MARK and whose w1 names
// the operation (the game inserts them where the N64 used framebuffer tricks)
#define S30_GFX_MARK 0x424C4E44u          // "BLND"
#define S30_MARK_BG_SAVE 0x50424753u      // "PBGS": copy the frame so far as the pause background
#define S30_MARK_BG_RESTORE 0x50424752u   // "PBGR": draw the saved pause background
#define S30_MARK_VISMONO 0x564D4F4Eu      // "VMON": VisMono, desaturate the frame (prim/env colors)
#define S30_MARK_MBLUR_BLEND 0x4D424C42u  // "MBLB": motion blur, previous frame over this one at prim alpha
#define S30_MARK_MBLUR_SAVE 0x4D424C53u   // "MBLS": motion blur, save this frame for the next

// The value of the debug environment variable <prefix>_<name>, or NULL
const char* S30_Getenv(const char* name);
// Set by S30Gfx_Configure (also used by the display and input)
void S30_SetEnvPrefix(const char* prefix);

#endif
