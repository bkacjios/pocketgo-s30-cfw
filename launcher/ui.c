#include "ui.h"
#include "../utils/osd_font.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define GAMEPAD_NAME "PocketGo S30 Gamepad"

// Purple of s30-display's overlays (0x8060e0), lightened for text on black
const SDL_Color UI_ACCENT = { 150, 120, 240, 255 };
const SDL_Color UI_TEXT = { 255, 255, 255, 255 };
const SDL_Color UI_DIM = { 175, 175, 175, 255 };

SDL_Renderer* gUiRenderer;

static SDL_Window* sWindow;
static SDL_Texture* sCanvas;
static SDL_Texture* sGlyphs; // the font's 95 glyphs in a row, white on transparent
static const char* sShotPath; // S30_UI_SHOT: desktop testing, see ui_video_open
/* Button glyphs are built once per letter (textures die with the renderer) */
static struct {
    const char* const* letter;
    SDL_Texture* tex;
} sButtons[3];


bool ui_init(void) {
    // SDL would turn SIGTERM into an SDL_QUIT event nobody reads: keep the
    // default action, so `killall s30-launcher` works
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(0) != 0) {
        fprintf(stderr, "ui: SDL init failed: %s\n", SDL_GetError());
        return false;
    }
    return ui_video_open();
}

/* Glyph strip for ui_text (tinted per call with the colour mod) */
static SDL_Texture* make_glyphs(void) {
    static Uint32 px[UI_CHAR_H][95 * UI_CHAR_W];
    SDL_Texture* t = SDL_CreateTexture(gUiRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
                                       95 * UI_CHAR_W, UI_CHAR_H);
    int c, x, y;

    if (t == NULL) {
        return NULL;
    }
    for (c = 0; c < 95; c++) {
        for (y = 0; y < UI_CHAR_H; y++) {
            for (x = 0; x < UI_CHAR_W; x++) {
                px[y][c * UI_CHAR_W + x] = kOsdFont[c][y] & (0x80 >> x) ? 0xffffffff : 0;
            }
        }
    }
    SDL_UpdateTexture(t, NULL, px, sizeof(px[0]));
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

/* /dev/fb0 to black, once the window is up (it hides fbcon). The boot splash
   stays there until then (etc/s30/rc leaves it, so the screen isn't black
   while the launcher starts); fbcon shows fb0 again whenever a KMS app exits,
   so it must not stay. Writes until the end of the framebuffer (ENOSPC). */
static void clear_fbcon(void) {
    static const char zero[64 * 1024];
    int fd = open("/dev/fb0", O_WRONLY | O_CLOEXEC);

    if (fd < 0) {
        return;
    }
    while (write(fd, zero, sizeof(zero)) > 0) {
    }
    close(fd);
}

bool ui_video_open(void) {
    // KMS only (no Wayland/X probing), GLES2 renderer (Mesa's desktop GL on
    // the Mali-400 has no render targets). S30_UI_SHOT=file.bmp (desktop
    // testing, with SDL_VIDEODRIVER set): a 320x480 window instead, and
    // every presented canvas saved to that file.
    sShotPath = getenv("S30_UI_SHOT");
    if (sShotPath == NULL) {
        SDL_SetHint(SDL_HINT_VIDEODRIVER, "kmsdrm");
    }
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "ui: SDL video: %s\n", SDL_GetError());
        return false;
    }
    sWindow = sShotPath != NULL
                  ? SDL_CreateWindow("s30", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, UI_H, UI_W, SDL_WINDOW_OPENGL)
                  : SDL_CreateWindow("s30", 0, 0, 0, 0, SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_OPENGL);
    if (sWindow == NULL) {
        fprintf(stderr, "ui: window: %s\n", SDL_GetError());
        return false;
    }
    gUiRenderer = SDL_CreateRenderer(sWindow, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    if (gUiRenderer == NULL) {
        fprintf(stderr, "ui: renderer: %s\n", SDL_GetError());
        return false;
    }
    // The window's first frame never shows (SDL's KMS backend sets the mode
    // with it, and the panel stays black until the next flip): a black one,
    // so the menu's first frame (ui_present) is the second and shows at once
    // instead of at the next redraw (up to 2 s, at boot and after an app)
    SDL_SetRenderDrawColor(gUiRenderer, 0, 0, 0, 255);
    SDL_RenderClear(gUiRenderer);
    SDL_RenderPresent(gUiRenderer);
    clear_fbcon();
    sCanvas = SDL_CreateTexture(gUiRenderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, UI_W, UI_H);
    sGlyphs = make_glyphs();
    if (sCanvas == NULL || sGlyphs == NULL) {
        fprintf(stderr, "ui: canvas: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetRenderDrawBlendMode(gUiRenderer, SDL_BLENDMODE_BLEND);
    return true;
}

void ui_video_close(void) {
    int i;

    for (i = 0; i < 3; i++) {
        SDL_DestroyTexture(sButtons[i].tex);
        sButtons[i].tex = NULL;
    }
    SDL_DestroyTexture(sGlyphs);
    SDL_DestroyTexture(sCanvas);
    SDL_DestroyRenderer(gUiRenderer);
    SDL_DestroyWindow(sWindow);
    sCanvas = NULL;
    sGlyphs = NULL;
    gUiRenderer = NULL;
    sWindow = NULL;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void ui_begin(void) {
    SDL_SetRenderTarget(gUiRenderer, sCanvas);
    SDL_SetRenderDrawColor(gUiRenderer, 0, 0, 0, 255);
    SDL_RenderClear(gUiRenderer);
}

/* The canvas to a BMP (S30_UI_SHOT), while it is the render target */
static void save_canvas(void) {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, UI_W, UI_H, 32, SDL_PIXELFORMAT_RGBA32);

    if (s != NULL) {
        if (SDL_RenderReadPixels(gUiRenderer, NULL, s->format->format, s->pixels, s->pitch) == 0) {
            SDL_SaveBMP(s, sShotPath);
        }
        SDL_FreeSurface(s);
    }
}

void ui_present(void) {
    int w, h;
    SDL_Rect dst;

    if (sShotPath != NULL) {
        save_canvas();
    }
    // Landscape canvas onto the portrait panel: canvas top to the panel's left
    SDL_SetRenderTarget(gUiRenderer, NULL);
    SDL_SetRenderDrawColor(gUiRenderer, 0, 0, 0, 255);
    SDL_RenderClear(gUiRenderer);
    SDL_GetRendererOutputSize(gUiRenderer, &w, &h);
    dst = (SDL_Rect){ w / 2 - UI_W / 2, h / 2 - UI_H / 2, UI_W, UI_H };
    SDL_RenderCopyEx(gUiRenderer, sCanvas, NULL, &dst, 270, NULL, SDL_FLIP_NONE);
    SDL_RenderPresent(gUiRenderer);
}

/* `text` in 8x16 cells from (x, y), cut off at maxW pixels */
void ui_text(const char* text, SDL_Color color, int x, int y, int maxW) {
    int off;

    SDL_SetTextureColorMod(sGlyphs, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(sGlyphs, color.a);
    for (off = 0; *text != '\0' && off < maxW; text++, off += UI_CHAR_W) {
        unsigned char c = (unsigned char)*text < 0x20 || (unsigned char)*text > 0x7e ? '?' : *text;
        int w = maxW - off < UI_CHAR_W ? maxW - off : UI_CHAR_W;
        SDL_Rect src = { (c - 0x20) * UI_CHAR_W, 0, w, UI_CHAR_H }, dst = { x + off, y, w, UI_CHAR_H };

        SDL_RenderCopy(gUiRenderer, sGlyphs, &src, &dst);
    }
}

int ui_text_width(const char* text) {
    return (int)strlen(text) * UI_CHAR_W;
}

void ui_fill(SDL_Rect r, SDL_Color c) {
    SDL_SetRenderDrawColor(gUiRenderer, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(gUiRenderer, &r);
}

void ui_box(SDL_Rect r, SDL_Color c, int thickness) {
    int i;

    SDL_SetRenderDrawColor(gUiRenderer, c.r, c.g, c.b, c.a);
    for (i = 0; i < thickness; i++) {
        SDL_Rect o = { r.x + i, r.y + i, r.w - 2 * i, r.h - 2 * i };
        SDL_RenderDrawRect(gUiRenderer, &o);
    }
}

/* Vertical gradient (two triangles, colours interpolated by the GPU) */
void ui_gradient(SDL_Rect r, SDL_Color top, SDL_Color bottom) {
    const float x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
    const SDL_Vertex v[4] = {
        { { x0, y0 }, top, { 0, 0 } },
        { { x1, y0 }, top, { 0, 0 } },
        { { x1, y1 }, bottom, { 0, 0 } },
        { { x0, y1 }, bottom, { 0, 0 } },
    };
    static const int idx[6] = { 0, 1, 2, 0, 2, 3 };

    SDL_RenderGeometry(gUiRenderer, NULL, v, 4, idx, 6);
}

void ui_button(const char* const* letter, int x, int y) {
    SDL_Rect dst = { x, y, BUTTON_SIZE, BUTTON_SIZE };
    int i;

    for (i = 0; i < 3; i++) {
        if (sButtons[i].letter == letter || sButtons[i].letter == NULL) {
            break;
        }
    }
    if (i == 3) {
        return;
    }
    if (sButtons[i].tex == NULL) {
        Uint32 px[BUTTON_SIZE][BUTTON_SIZE];
        int gx, gy;

        for (gy = 0; gy < BUTTON_SIZE; gy++) {
            for (gx = 0; gx < BUTTON_SIZE; gx++) {
                Uint32 rgb = button_letter(letter, gx, gy) ? BUTTON_DARK : BUTTON_LIGHT;
                px[gy][gx] = (Uint32)(button_coverage(gx, gy) * 255.0f + 0.5f) << 24 | rgb;
            }
        }
        sButtons[i].letter = letter;
        sButtons[i].tex = SDL_CreateTexture(gUiRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
                                            BUTTON_SIZE, BUTTON_SIZE);
        if (sButtons[i].tex == NULL) {
            return;
        }
        SDL_UpdateTexture(sButtons[i].tex, NULL, px, sizeof(px[0]));
        SDL_SetTextureBlendMode(sButtons[i].tex, SDL_BLENDMODE_BLEND);
    }
    SDL_RenderCopy(gUiRenderer, sButtons[i].tex, NULL, &dst);
}

SDL_Texture* ui_image_load(const char* path, int maxDim, int* w, int* h) {
    int iw, ih, n, f = 1;
    unsigned char* px = stbi_load(path, &iw, &ih, &n, 4);
    SDL_Texture* t;

    if (px == NULL) {
        return NULL;
    }
    if (maxDim > 0) {
        int big = iw > ih ? iw : ih;
        f = (big + maxDim - 1) / maxDim;
    }
    if (f > 1) {
        // Box filter: each output pixel is the mean of an f x f block
        int ow = iw / f, oh = ih / f, x, y, i, j, c;
        unsigned char* small = malloc((size_t)ow * oh * 4);

        if (small == NULL || ow == 0 || oh == 0) {
            free(small);
            stbi_image_free(px);
            return NULL;
        }
        for (y = 0; y < oh; y++) {
            for (x = 0; x < ow; x++) {
                unsigned sum[4] = { 0, 0, 0, 0 };
                for (j = 0; j < f; j++) {
                    const unsigned char* row = px + ((size_t)(y * f + j) * iw + (size_t)x * f) * 4;
                    for (i = 0; i < f; i++) {
                        for (c = 0; c < 4; c++) {
                            sum[c] += row[i * 4 + c];
                        }
                    }
                }
                for (c = 0; c < 4; c++) {
                    small[((size_t)y * ow + x) * 4 + c] = sum[c] / (f * f);
                }
            }
        }
        stbi_image_free(px);
        px = small;
        iw = ow;
        ih = oh;
    }
    t = SDL_CreateTexture(gUiRenderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, iw, ih);
    if (t != NULL) {
        SDL_UpdateTexture(t, NULL, px, iw * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    }
    if (f > 1) {
        free(px);
    } else {
        stbi_image_free(px);
    }
    *w = iw;
    *h = ih;
    return t;
}

void ui_image_fit(SDL_Texture* t, int w, int h, SDL_Rect box) {
    SDL_Rect dst;

    if (w * box.h > h * box.w) { // wider than the box
        dst.w = box.w;
        dst.h = h * box.w / w;
    } else {
        dst.h = box.h;
        dst.w = w * box.h / h;
    }
    dst.x = box.x + (box.w - dst.w) / 2;
    dst.y = box.y + (box.h - dst.h) / 2;
    SDL_RenderCopy(gUiRenderer, t, NULL, &dst);
}

int ui_pad_open(void) {
    char path[64], name[128];
    int i;

    for (i = 0; i < 16; i++) {
        int fd;

        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        name[0] = 0;
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        if (strcmp(name, GAMEPAD_NAME) == 0) {
            return fd;
        }
        close(fd);
    }
    return -1;
}

bool ui_pad_poll(int fd, bool held[], bool pressed[], const int codes[], int ncodes) {
    struct input_event ev[32];
    ssize_t n;
    bool changed = false;
    int i, k;

    if (pressed != NULL) {
        memset(pressed, 0, ncodes * sizeof(pressed[0]));
    }
    while (fd >= 0 && (n = read(fd, ev, sizeof(ev))) > 0) {
        for (i = 0; i < n / (int)sizeof(ev[0]); i++) {
            if (ev[i].type != EV_KEY) {
                continue;
            }
            for (k = 0; k < ncodes; k++) {
                if (codes[k] == ev[i].code) {
                    held[k] = ev[i].value != 0;
                    if (pressed != NULL && ev[i].value == 1) {
                        pressed[k] = true;
                    }
                    changed = true;
                }
            }
        }
    }
    return changed;
}

void ui_pad_axes(int fd, int* x, int* y) {
    struct input_absinfo abs;

    if (fd >= 0 && ioctl(fd, EVIOCGABS(ABS_X), &abs) == 0) {
        *x = abs.value;
    }
    if (fd >= 0 && ioctl(fd, EVIOCGABS(ABS_Y), &abs) == 0) {
        *y = abs.value;
    }
}
