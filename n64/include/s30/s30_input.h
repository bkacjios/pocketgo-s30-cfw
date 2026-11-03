#ifndef S30_INPUT_H
#define S30_INPUT_H

// N64 controller from the S30 gamepad (n64/src/input.c)

#include <stdint.h>

// N64 controller button bits
enum {
    S30_N64_A = 0x8000,
    S30_N64_B = 0x4000,
    S30_N64_Z = 0x2000,
    S30_N64_START = 0x1000,
    S30_N64_DUP = 0x0800,
    S30_N64_DDOWN = 0x0400,
    S30_N64_DLEFT = 0x0200,
    S30_N64_DRIGHT = 0x0100,
    S30_N64_L = 0x0020,
    S30_N64_R = 0x0010,
    S30_N64_CUP = 0x0008,
    S30_N64_CDOWN = 0x0004,
    S30_N64_CLEFT = 0x0002,
    S30_N64_CRIGHT = 0x0001,
};

// The game's layout: an evdev key code (BTN_* from linux/input.h) and the N64
// buttons it presses
typedef struct S30ButtonMap {
    uint16_t code;
    uint16_t button;
} S30ButtonMap;

// One N64 controller state built from the S30 gamepad
typedef struct S30Pad {
    uint16_t button; // S30_N64_* bits
    int8_t stick_x;
    int8_t stick_y;
} S30Pad;

void S30Input_Init(const S30ButtonMap* map, int count);
void S30Input_Read(S30Pad* pad);

#endif
