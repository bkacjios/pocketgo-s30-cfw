/*
 * Audio output (s30_audio.h): the game's audio update runs on a thread of its
 * own and renders a frame of 32 kHz stereo (S30Audio_Play, separate L/R, or
 * S30Audio_PlayInterleaved); the thread then writes it to ALSA, outside the
 * lock. snd_pcm_writei blocks while the device buffer is full, so the update
 * runs at the rate the codec consumes samples (about 60 per second), like the
 * N64's AI-driven audio thread.
 */
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "s30_audio.h"
#include "s30_gfx.h"

#define S30_AUDIO_RATE 32000
#define S30_AUDIO_PERIOD 512 // frames
#define S30_AUDIO_PERIODS 4  // buffer = 64 ms
#define MAX_FRAMES 1024      // per update

int gS30EnableAudio = 1;

static snd_pcm_t* sPcm;
static snd_pcm_uframes_t sBufferFrames;
static void (*sUpdate)(void);

static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
// The frame the update rendered, written to ALSA after unlocking
static int16_t sPending[2 * MAX_FRAMES];
static int32_t sPendingFrames;

void S30Audio_Lock(void) {
    pthread_mutex_lock(&sLock);
}

void S30Audio_Unlock(void) {
    pthread_mutex_unlock(&sLock);
}

static void write_frames(const int16_t* samples, int32_t frames) {
    int32_t done = 0;

    if (sPcm == NULL) {
        return;
    }
    while (done < frames) {
        snd_pcm_sframes_t written = snd_pcm_writei(sPcm, samples + 2 * done, frames - done);

        if (written < 0) {
            // Underrun (-EPIPE) or suspend: recover and drop the rest
            snd_pcm_recover(sPcm, written, 1);
            return;
        }
        done += written;
    }
}

static void* audio_thread(void* arg) {
    (void)arg;
    for (;;) {
        int32_t frames;

        pthread_mutex_lock(&sLock);
        sPendingFrames = 0;
        sUpdate();
        frames = sPendingFrames;
        pthread_mutex_unlock(&sLock);
        if (frames == 0) {
            // Nothing rendered (a skipped or resetting update): don't spin on the lock
            usleep(1000);
            continue;
        }
        // Blocks while the device buffer is full: the game thread runs meanwhile
        write_frames(sPending, frames);
    }
    return NULL;
}

int S30Audio_Open(void) {
    snd_pcm_hw_params_t* hw;
    snd_pcm_uframes_t period = S30_AUDIO_PERIOD;
    unsigned int rate = S30_AUDIO_RATE;
    int err;

    if (!gS30EnableAudio || S30_Getenv("NOAUDIO") != NULL) {
        return 0;
    }
    err = snd_pcm_open(&sPcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        printf("audio: snd_pcm_open: %s, audio disabled\n", snd_strerror(err));
        sPcm = NULL;
        return 0;
    }
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(sPcm, hw);
    snd_pcm_hw_params_set_access(sPcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(sPcm, hw, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(sPcm, hw, 2);
    snd_pcm_hw_params_set_rate_near(sPcm, hw, &rate, NULL);
    snd_pcm_hw_params_set_period_size_near(sPcm, hw, &period, NULL);
    sBufferFrames = period * S30_AUDIO_PERIODS;
    snd_pcm_hw_params_set_buffer_size_near(sPcm, hw, &sBufferFrames);
    err = snd_pcm_hw_params(sPcm, hw);
    if (err < 0) {
        printf("audio: snd_pcm_hw_params: %s, audio disabled\n", snd_strerror(err));
        snd_pcm_close(sPcm);
        sPcm = NULL;
        return 0;
    }
    printf("audio: %u Hz, period %lu, buffer %lu frames\n", rate, (unsigned long)period,
           (unsigned long)sBufferFrames);
    return 1;
}

void S30Audio_Start(void (*update)(void)) {
    pthread_t thread;

    sUpdate = update;
    pthread_create(&thread, NULL, audio_thread, NULL);
    pthread_detach(thread);
}

void S30Audio_Init(void (*update)(void)) {
    S30Audio_Open();
    S30Audio_Start(update);
}

uint32_t S30Audio_QueuedFrames(void) {
    snd_pcm_sframes_t avail;

    if (sPcm == NULL) {
        return 0;
    }
    avail = snd_pcm_avail_update(sPcm);
    if (avail < 0 || (snd_pcm_uframes_t)avail > sBufferFrames) {
        return 0;
    }
    return sBufferFrames - avail;
}

void S30Audio_PlayInterleaved(const int16_t* samples, int32_t frames) {
    if (frames > MAX_FRAMES) {
        frames = MAX_FRAMES;
    }
    memcpy(sPending, samples, frames * 2 * sizeof(int16_t));
    sPendingFrames = frames;
}

void S30Audio_Play(const int16_t* left, const int16_t* right, int32_t frames) {
    int32_t i;

    if (frames > MAX_FRAMES) {
        frames = MAX_FRAMES;
    }
    for (i = 0; i < frames; i++) {
        sPending[2 * i] = left[i];
        sPending[2 * i + 1] = right[i];
    }
    sPendingFrames = frames;
}
