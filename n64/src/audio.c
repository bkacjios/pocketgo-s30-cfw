/*
 * Audio output (s30_audio.h): the game's audio update runs on a thread of its
 * own and renders a frame of stereo at 32 kHz or the configured rate (S30Audio_Play, separate L/R, or
 * S30Audio_PlayInterleaved); the thread then writes it to ALSA, outside the
 * lock. snd_pcm_writei blocks while the device buffer is full, so the update
 * runs at the rate the codec consumes samples (about 60 per second), like the
 * N64's AI-driven audio thread.
 *
 * S30Audio_Configure changes two things for games that need it:
 * - the sample rate: Mario Kart 64 mixes at 26800 Hz, which the codec can't
 *   run at (7350-192000, standard rates only). ALSA's plug converts it; its
 *   own choice of hardware rate would be 22050, so the hardware side is
 *   pinned to 32 kHz;
 * - a fixed update rate: the update runs on the monotonic clock instead of
 *   being paced by full-buffer writes, like the N64's vblank-driven audio
 *   thread. Games that size each frame from the AI length (target - samples
 *   still queued + a margin) need this: with blocking writes the queue always
 *   reads full, every frame comes out the minimum length and the music plays
 *   fast. Here S30Audio_QueuedFrames reads the queue less a fixed latency
 *   (prefilled with silence), so the game's own feedback holds it there.
 */
#include <time.h>
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

// S30Audio_Configure: the game's sample rate, and the update period (0: paced
// by blocking writes)
static unsigned int sRate = S30_AUDIO_RATE;
static int64_t sPeriodNs;
// Paced mode: smaller periods, as the queue can only be read to a period
// (the codec's DMA reports its position per period; at 512 frames the game's
// buffer sizing swung between its limits and the queue ran dry), and frames
// kept queued (the AI length reads the rest)
#define PACED_PERIOD 256
#define PACED_PERIODS 8
#define PACED_LATENCY (PACED_PERIOD * 4)

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

static int sUnderruns;

int S30Audio_Underruns(void) {
    return sUnderruns;
}

// Paced mode: the frames kept queued, as silence (at the start and after an
// underrun, which empties the queue)
static void prefill(void) {
    static const int16_t silence[2 * PACED_LATENCY];

    if (sPeriodNs != 0) {
        snd_pcm_writei(sPcm, silence, PACED_LATENCY);
    }
}

static void write_frames(const int16_t* samples, int32_t frames) {
    int32_t done = 0;

    if (sPcm == NULL) {
        return;
    }
    while (done < frames) {
        snd_pcm_sframes_t written = snd_pcm_writei(sPcm, samples + 2 * done, frames - done);

        if (written < 0) {
            // Underrun (-EPIPE; also the process stopped, as under the system
            // menu) or suspend: recover, and in paced mode queue the latency
            // again, else the game's AI length reads short of it from then on
            // and the music stutters
            sUnderruns++;
            if (snd_pcm_recover(sPcm, written, 1) < 0) {
                return;
            }
            prefill();
            continue;
        }
        done += written;
    }
}

static int64_t now_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

// Paced mode: sleeps until the next update is due
static void wait_tick(void) {
    static int64_t next;
    int64_t now = now_ns();
    struct timespec ts;

    if (next == 0 || now - next > 4 * sPeriodNs) {
        next = now; // first update, or far behind (a stall): start over
    }
    if (next > now) {
        ts.tv_sec = next / 1000000000;
        ts.tv_nsec = next % 1000000000;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
    }
    next += sPeriodNs;
}

static void* audio_thread(void* arg) {
    (void)arg;
    if (sPcm != NULL) {
        prefill();
    }
    for (;;) {
        int32_t frames;

        if (sPeriodNs != 0) {
            wait_tick();
        }
        pthread_mutex_lock(&sLock);
        sPendingFrames = 0;
        sUpdate();
        frames = sPendingFrames;
        pthread_mutex_unlock(&sLock);
        if (frames == 0 && sPeriodNs == 0) {
            // Nothing rendered (a skipped or resetting update): don't spin on the lock
            usleep(1000);
            continue;
        }
        // Blocks while the device buffer is full: the game thread runs meanwhile
        if (frames != 0) {
            write_frames(sPending, frames);
        }
    }
    return NULL;
}

// "default" (plug on the codec), or at a rate the codec lacks the same with
// its hardware side at 32 kHz
static int open_pcm(void) {
    static const char conf[] = "pcm.s30 { type plug slave { pcm \"hw:0,0\" rate 32000 } }";
    snd_config_t* top;
    snd_input_t* in;
    int err;

    if (sRate == S30_AUDIO_RATE) {
        return snd_pcm_open(&sPcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    }
    // The system configuration (it defines hw), plus pcm.s30
    if ((err = snd_config_update()) < 0 || (err = snd_config_copy(&top, snd_config)) < 0) {
        return err;
    }
    if ((err = snd_input_buffer_open(&in, conf, -1)) < 0) {
        snd_config_delete(top);
        return err;
    }
    err = snd_config_load(top, in);
    snd_input_close(in);
    if (err >= 0) {
        err = snd_pcm_open_lconf(&sPcm, "s30", SND_PCM_STREAM_PLAYBACK, 0, top);
    }
    snd_config_delete(top);
    return err;
}

int S30Audio_Open(void) {
    snd_pcm_hw_params_t* hw;
    snd_pcm_uframes_t period = sPeriodNs != 0 ? PACED_PERIOD : S30_AUDIO_PERIOD;
    unsigned int rate = sRate;
    int err;

    if (!gS30EnableAudio || S30_Getenv("NOAUDIO") != NULL) {
        return 0;
    }
    err = open_pcm();
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
    sBufferFrames = period * (sPeriodNs != 0 ? PACED_PERIODS : S30_AUDIO_PERIODS);
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

void S30Audio_Configure(int32_t rate, float updateHz) {
    sRate = rate > 0 ? (unsigned int)rate : S30_AUDIO_RATE;
    sPeriodNs = updateHz > 0 ? (int64_t)(1e9 / updateHz) : 0;
}

void S30Audio_Init(void (*update)(void)) {
    S30Audio_Open();
    S30Audio_Start(update);
}

uint32_t S30Audio_QueuedFrames(void) {
    snd_pcm_sframes_t avail;
    int64_t queued;

    if (sPcm == NULL) {
        return 0;
    }
    avail = snd_pcm_avail_update(sPcm);
    if (avail < 0 || (snd_pcm_uframes_t)avail > sBufferFrames) {
        return 0;
    }
    queued = (int64_t)(sBufferFrames - avail);
    if (sPeriodNs != 0) {
        queued -= PACED_LATENCY;
        if (queued < 0) {
            return 0;
        }
    }
    return (uint32_t)queued;
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
