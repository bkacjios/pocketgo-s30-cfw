#ifndef S30_AUDIO_H
#define S30_AUDIO_H

// Audio output (n64/src/audio.c): the game's audio update runs on a thread
// of its own, renders a frame of 32 kHz stereo with S30Audio_Play or
// S30Audio_PlayInterleaved, and the thread writes it to ALSA. The write blocks
// while the device buffer is full, which paces the update like the N64's
// audio interrupt.

#include <stdint.h>

extern int gS30EnableAudio; // 0: no audio (also <prefix>_NOAUDIO)

// Opens the device (0 if there is no audio), then starts the thread calling
// `update` (S30Audio_Init does both). Each update runs with the audio lock held.
int S30Audio_Open(void);
void S30Audio_Start(void (*update)(void));
void S30Audio_Init(void (*update)(void));

// Frames still queued in the device (the N64's AI length, in frames)
uint32_t S30Audio_QueuedFrames(void);
// The update's output: separate L/R, or interleaved stereo
void S30Audio_Play(const int16_t* left, const int16_t* right, int32_t frames);
void S30Audio_PlayInterleaved(const int16_t* samples, int32_t frames);

// Held by the audio thread while it updates; the game thread can take it
// around its own audio work: on the N64 the two never ran at the same time
void S30Audio_Lock(void);
void S30Audio_Unlock(void);

#endif
