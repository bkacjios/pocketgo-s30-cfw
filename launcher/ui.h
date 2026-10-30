#ifndef S30_UI_H
#define S30_UI_H

/* Shared UI for the S30 launcher apps: SDL2 on KMS with the GLES2 renderer,
   a 480x320 landscape canvas rotated onto the portrait panel, Ozone fonts,
   the gamepad from evdev. */
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdbool.h>

#define UI_W 480
#define UI_H 320

extern SDL_Renderer* gUiRenderer;
extern TTF_Font* gUiFontBold;
extern TTF_Font* gUiFontRegular;
extern TTF_Font* gUiFontSmall; // bold, 12 px

extern const SDL_Color UI_ACCENT;
extern const SDL_Color UI_TEXT;
extern const SDL_Color UI_DIM;

bool ui_init(void);           // fonts + display
bool ui_video_open(void);     // (re)acquire the display
void ui_video_close(void);    // release it (e.g. while another app runs)
void ui_begin(void);          // start drawing on the canvas (cleared black)
void ui_present(void);        // canvas onto the panel
void ui_text(TTF_Font* font, const char* text, SDL_Color color, int x, int y, int maxW);

/* Gamepad (evdev "PocketGo S30 Gamepad") */
int ui_pad_open(void);                 // fd or -1
bool ui_pad_poll(int fd, bool held[], const int codes[], int ncodes); // true if a key changed
void ui_pad_axes(int fd, int* x, int* y); // current raw ABS_X / ABS_Y

#endif
