/* Face-button glyphs shared by s30-display (menus) and the launcher
   (footer hints): a 14 px disc, its edge anti-aliased by pixel coverage,
   with the button's letter (a 6x8 bitmap) cut in dark. */
#ifndef S30_OSD_BUTTON_H
#define S30_OSD_BUTTON_H

#include <math.h>

#define BUTTON_SIZE 14
#define BUTTON_LIGHT 0xc8c8c8
#define BUTTON_DARK 0x101018

static const char* const kLetterA[8] = { ".####.", "##..##", "##..##", "##..##", "######", "##..##", "##..##", "##..##" };
static const char* const kLetterB[8] = { "#####.", "##..##", "##..##", "#####.", "##..##", "##..##", "##..##", "#####." };
static const char* const kLetterX[8] = { "##..##", "##..##", ".####.", "..##..", "..##..", ".####.", "##..##", "##..##" };

/* How much of pixel (x, y) of the glyph the disc covers, 0..1 */
static inline float button_coverage(int x, int y) {
	float d = sqrtf((x - 6.5f) * (x - 6.5f) + (y - 6.5f) * (y - 6.5f));
	float a = 7.0f + 0.5f - d;
	return a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
}

/* Whether pixel (x, y) is on the letter */
static inline int button_letter(const char* const* letter, int x, int y) {
	int lx = x - 4, ly = y - 3;
	return lx >= 0 && lx < 6 && ly >= 0 && ly < 8 && letter[ly][lx] == '#';
}

#endif
