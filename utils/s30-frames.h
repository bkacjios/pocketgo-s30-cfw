/*
 * Frame timing ring shared by libs30lease.so (writes: one timestamp per page
 * flip the app queues) and s30-display (creates it; reads it for the
 * performance overlay's FPS and frame time graph).
 */
#ifndef S30_FRAMES_H
#define S30_FRAMES_H

#include <stdint.h>

#define S30_FRAMES_PATH "/run/s30-frames"
#define S30_FRAMES_N 256

typedef struct {
	volatile uint32_t count; // frames ever marked; frame i is at ns[i % S30_FRAMES_N]
	volatile uint32_t pid;   // process that marked the last one
	volatile uint64_t ns[S30_FRAMES_N]; // CLOCK_MONOTONIC
} S30Frames;

#endif
