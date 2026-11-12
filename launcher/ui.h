#ifndef S30_UI_H
#define S30_UI_H

/* Shared UI for the S30 launcher apps: SDL2 on KMS with the GLES2 renderer,
   a 480x320 landscape canvas rotated onto the portrait panel, text in the
   system menu's 8x16 font (utils/osd_font.h), images through stb_image,
   the gamepad from evdev. */
#include <SDL2/SDL.h>
#include "../utils/osd_button.h"
#include <stdbool.h>

#define UI_W 480
#define UI_H 320

#define UI_CHAR_W 8 // text cells are 8x16
#define UI_CHAR_H 16

extern SDL_Renderer* gUiRenderer;

extern const SDL_Color UI_ACCENT;
extern const SDL_Color UI_TEXT;
extern const SDL_Color UI_DIM;

bool ui_init(void);           // display
bool ui_video_open(void);     // (re)acquire the display
void ui_video_close(void);    // release it (e.g. while another app runs)
void ui_begin(void);          // start drawing on the canvas (cleared black)
void ui_present(void);        // canvas onto the panel
void ui_text(const char* text, SDL_Color color, int x, int y, int maxW); // ASCII; others '?'
int ui_text_width(const char* text);

/* Drawing */
void ui_fill(SDL_Rect r, SDL_Color c);
void ui_box(SDL_Rect r, SDL_Color c, int thickness);
void ui_gradient(SDL_Rect r, SDL_Color top, SDL_Color bottom);
void ui_button(const char* const* letter, int x, int y); // kLetterA/B/X (utils/osd_button.h), 14x14

/* Images (PNG, JPEG, BMP): a texture, or NULL. With maxDim > 0 the picture is
   downsampled (box filter, whole factors) until its larger side fits:
   thumbnails stay small in memory. *w, *h get the texture's size. */
SDL_Texture* ui_image_load(const char* path, int maxDim, int* w, int* h);
void ui_image_fit(SDL_Texture* t, int w, int h, SDL_Rect box); // scaled to fit, centred

/* Gamepad (evdev "PocketGo S30 Gamepad") */
int ui_pad_open(void);                 // fd or -1
/* Reads the pending events: held[] is the keys' state, pressed[] (may be
   NULL) is set for keys pressed since the last call, even if released again
   already (a short tap inside one poll interval). True if a key changed. */
bool ui_pad_poll(int fd, bool held[], bool pressed[], const int codes[], int ncodes);
void ui_pad_axes(int fd, int* x, int* y); // current raw ABS_X / ABS_Y

#endif
