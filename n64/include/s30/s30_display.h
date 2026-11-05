#ifndef S30_DISPLAY_H
#define S30_DISPLAY_H

#include <GLES2/gl2.h>

// 3D framebuffer: the whole landscape screen (3:2; the game's 4:3 picture is
// its centre, see gfx.c WIDE_PAD)
#define S30_FB_W 480
#define S30_FB_H 320
// 2D layer: the N64's native 320x240 plus 20 pixels each side (the
// widescreen margins), so rectangles (HUD, text, logos) are drawn 1:1 and
// composited onto the 3D image with sharp-bilinear scaling
#define S30_2D_W 360
#define S30_2D_H 240

// The refresh rate to run the panel at (before S30Display_Init): 60 for N64
// NTSC games (the panel's own mode is 61.8 Hz). <prefix>_REFRESH overrides it.
void S30Display_SetRefresh(float hz);
void S30Display_Init(void);
// Starts a frame: clears the 2D layer
void S30Display_BeginFrame(void);
// Binds the 3D framebuffer, compositing pending 2D first (keeps draw order);
// returns 1 if a composite ran (GL state changed)
int S30Display_Bind3D(void);
// Binds the 2D layer (premultiplied alpha, transparent where nothing drawn)
void S30Display_Bind2D(void);
// Pause background: copies the 3D image (2D composited in) / draws the copy
// as the 3D image. Both change GL state.
void S30Display_SaveBackground(void);
void S30Display_RestoreBackground(void);
// A rectangle of the 3D image ((x0, y0)-(x1, y1), framebuffer pixels from the
// top left) into *tex, w x h, top row first (created with *fbo on first use).
// Changes GL state.
void S30Display_CopyRect(GLuint* tex, GLuint* fbo, int w, int h, float x0, float y0, float x1, float y1);
// VisMono: desaturates the 3D image (2D composited in) toward prim/env (RGBA,
// RGB in 0..1). Changes GL state.
void S30Display_VisMono(const float prim[4], const float env[4]);
// Motion blur (MM's Play_DrawMotionBlur): blends the saved previous frame
// over the 3D image at alpha (0-1), and saves the 3D image for the next frame
void S30Display_MotionBlurBlend(float alpha);
void S30Display_MotionBlurSave(void);
// Depth probe in result slot `slot`: whether a point at 3D framebuffer pixel
// (x, y from the bottom) and NDC depth z passes LEQUAL against this frame's
// depth buffer. Tested at the end of the frame; S30Display_ReadProbes, after
// that frame, returns the slots' results (1 = visible) and their count.
// Reading changes GL state.
void S30Display_QueueProbe(int slot, int x, int y, float z);
int S30Display_ReadProbes(unsigned char* out, int max);
// The caller bound its own framebuffer: the next Bind3D/Bind2D rebinds
void S30Display_Unbind(void);
// Presents the frame (rotated, pillarboxed) and paces to 20 fps
void S30Display_EndFrame(void);

// Replacement texture: a texture loaded from `src` draws `pixels` (w x h, both powers of 2; RGBA8888, or RGB565 if
// `rgb565`) instead, at their own resolution. `pixels` is malloc'd and handed over: it is freed once on the GPU. (gfx.c)
void S30Gfx_TextureOverride(const void* src, int w, int h, int rgb565, void* pixels);
// Frames shown late since the last call, and the refreshes they lost
void S30Display_TakeLateFrames(int* frames, int* vblanks);
GLuint S30Display_LinkProgram(const char* vs, const char* fs);

#endif
