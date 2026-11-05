#include "ui.h"
#include "../utils/osd_font.h"

#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
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

bool ui_init(void) {
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
    // the Mali-400 has no render targets)
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "kmsdrm");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "ui: SDL video: %s\n", SDL_GetError());
        return false;
    }
    sWindow = SDL_CreateWindow("s30", 0, 0, 0, 0, SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_OPENGL);
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
    return true;
}

void ui_video_close(void) {
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

void ui_present(void) {
    int w, h;
    SDL_Rect dst;

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

bool ui_pad_poll(int fd, bool held[], const int codes[], int ncodes) {
    struct input_event ev[32];
    ssize_t n;
    bool changed = false;
    int i, k;

    while (fd >= 0 && (n = read(fd, ev, sizeof(ev))) > 0) {
        for (i = 0; i < n / (int)sizeof(ev[0]); i++) {
            if (ev[i].type != EV_KEY) {
                continue;
            }
            for (k = 0; k < ncodes; k++) {
                if (codes[k] == ev[i].code) {
                    held[k] = ev[i].value != 0;
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
