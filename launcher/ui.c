#include "ui.h"

#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define FONT_DIR "/usr/share/libretro/assets/ozone"
#define GAMEPAD_NAME "PocketGo S30 Gamepad"

// Purple of s30-display's overlays (0x8060e0), lightened for text on black
const SDL_Color UI_ACCENT = { 150, 120, 240, 255 };
const SDL_Color UI_TEXT = { 255, 255, 255, 255 };
const SDL_Color UI_DIM = { 175, 175, 175, 255 };

SDL_Renderer* gUiRenderer;
TTF_Font* gUiFontBold;
TTF_Font* gUiFontRegular;
TTF_Font* gUiFontSmall;

static SDL_Window* sWindow;
static SDL_Texture* sCanvas;

bool ui_init(void) {
    if (SDL_Init(0) != 0 || TTF_Init() != 0) {
        fprintf(stderr, "ui: SDL init failed: %s\n", SDL_GetError());
        return false;
    }
    gUiFontBold = TTF_OpenFont(FONT_DIR "/bold.ttf", 16);
    gUiFontRegular = TTF_OpenFont(FONT_DIR "/regular.ttf", 16);
    gUiFontSmall = TTF_OpenFont(FONT_DIR "/bold.ttf", 12);
    if (gUiFontBold == NULL || gUiFontRegular == NULL || gUiFontSmall == NULL) {
        fprintf(stderr, "ui: fonts in %s: %s\n", FONT_DIR, TTF_GetError());
        return false;
    }
    return ui_video_open();
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
    sCanvas = SDL_CreateTexture(gUiRenderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, UI_W, UI_H);
    if (sCanvas == NULL) {
        fprintf(stderr, "ui: canvas: %s\n", SDL_GetError());
        return false;
    }
    return true;
}

void ui_video_close(void) {
    SDL_DestroyTexture(sCanvas);
    SDL_DestroyRenderer(gUiRenderer);
    SDL_DestroyWindow(sWindow);
    sCanvas = NULL;
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

void ui_text(TTF_Font* font, const char* text, SDL_Color color, int x, int y, int maxW) {
    SDL_Surface* s;
    SDL_Texture* t;
    SDL_Rect src, dst;

    if (text[0] == 0 || (s = TTF_RenderUTF8_Blended(font, text, color)) == NULL) {
        return;
    }
    t = SDL_CreateTextureFromSurface(gUiRenderer, s);
    src = (SDL_Rect){ 0, 0, s->w < maxW ? s->w : maxW, s->h };
    dst = (SDL_Rect){ x, y, src.w, src.h };
    SDL_RenderCopy(gUiRenderer, t, &src, &dst);
    SDL_DestroyTexture(t);
    SDL_FreeSurface(s);
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
