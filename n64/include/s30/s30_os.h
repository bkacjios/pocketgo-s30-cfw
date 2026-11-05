#ifndef S30_OS_H
#define S30_OS_H

// libultra's OS for the N64 ports, on Linux (n64/src/os.c): threads, message
// queues, events, time and timers, the VI, the RCP's task queue, the
// controllers, PI DMA (the ROM image, EEPROM or SRAM saves) and the audio
// interface. A port opts in with N64LIB_OS := 1 before including n64.mk, and
// keeps only what is its own (overlays, its data, its stubs).
//
// Threads are pthreads, but one N64 thread runs at a time, as on the N64's
// CPU: the highest-priority ready thread, until it blocks, stops itself, or
// a thread of higher priority becomes ready. What the N64 did in interrupts
// (VI retrace, RCP task done, timers) comes from the library's own threads;
// when that readies a thread above the running one, it takes the CPU at once
// and the preempted thread waits at its next OS call.
//
// The RCP is the process's main thread (it owns the GL context): after
// starting the game's threads, main() calls S30Os_RunRcp. A graphics task's
// display list is drawn and shown (pc_process_displaylist), then the SP and DP
// events are posted; an audio task only posts the SP event (the game's audio
// commands ran on the software mixer, s30_mixer.h, as they were built).
//
// Debug (environment, with the renderer's prefix, s30_gfx.h): <PREFIX>_STATS
// prints the RCP tasks and the thread on the CPU every 2 seconds;
// <PREFIX>_AUDIODBG the audio's loudest sample, queue and underruns every
// second; <PREFIX>_AUDIODUMP writes the audio to /tmp/<prefix>_audio.raw (s16
// stereo). SIGQUIT prints where every N64 thread is (s30_app.h).

typedef enum {
    S30_SAVE_NONE,
    S30_SAVE_EEPROM_4K,  // 512 bytes
    S30_SAVE_EEPROM_16K, // 2 KB
    S30_SAVE_SRAM,       // 32 KB, through the PI handle of type DEVICE_TYPE_SRAM
} S30SaveType;

typedef struct {
    const char* romPath;  // the ROM image PI DMA reads (cartridge addresses & 0x0FFFFFFF), or NULL
    const char* savePath; // the save file (written whole when a write changes it)
    S30SaveType saveType;
    // Controller 1's Controller Pak, a 32 KB image (NULL: none), for ports
    // that build libultra's pak code (osPfs*, __osContRamRead/Write)
    const char* pakPath;
    // 0: osCreateThread/osStartThread/... do nothing (ports that run the
    // game's loops themselves); 1: the game's threads run, scheduled as above
    int threads;
    // 1 (with threads 0): no thread to wait for, so a receive from an empty
    // queue or a send to a full one fails at once (-1), as does one on a NULL
    // queue (OoT and MM wait for their scheduler that way, and go on)
    int noWait;
    // 1: osCreateViManager starts the VI's 60 Hz retrace messages
    int vi;
    // 1: osAiGetLength is the audio queued (for games that size their frames
    // from it, with S30Audio_Configure's update rate); 0: always 0 (the next
    // buffer is always a whole frame)
    int aiLength;
    // Called before each ROM read with the RAM it overwrites, or NULL
    void (*beforeDma)(void* ram, unsigned int size);
    // <PREFIX>_AUDIODBG: called after the library's line each second, for the
    // game's own counters, or NULL
    void (*audioDebug)(void);
} S30OsConfig;

// Before the game's osInitialize (the config is copied)
void S30Os_Init(const S30OsConfig* config);

// The RCP, on the main thread: never returns
void S30Os_RunRcp(void);

#endif
