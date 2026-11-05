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
// Before S30Audio_Open: the rate the game renders at (ALSA converts it if the
// codec lacks it), and a fixed update rate (0: as fast as the device takes
// samples). With an update rate, S30Audio_QueuedFrames is the queue beyond a
// fixed latency: the N64's AI length, for games that size their buffers from
// it (see audio.c).
void S30Audio_Configure(int32_t rate, float updateHz);
void S30Audio_Start(void (*update)(void));
void S30Audio_Init(void (*update)(void));

// Frames still queued in the device (the N64's AI length, in frames; see
// S30Audio_Configure)
uint32_t S30Audio_QueuedFrames(void);
// Device underruns so far (debug output)
int S30Audio_Underruns(void);
// The update's output: separate L/R, or interleaved stereo
void S30Audio_Play(const int16_t* left, const int16_t* right, int32_t frames);
void S30Audio_PlayInterleaved(const int16_t* samples, int32_t frames);

// Held by the audio thread while it updates; the game thread can take it
// around its own audio work: on the N64 the two never ran at the same time
void S30Audio_Lock(void);
void S30Audio_Unlock(void);

#endif
