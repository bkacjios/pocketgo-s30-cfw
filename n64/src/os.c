/*
 * libultra's OS for the N64 ports, on Linux (s30_os.h): threads, message
 * queues, events, time and timers, the VI, the RCP's task queue, the
 * controllers and paks, PI DMA (ROM image, EEPROM, SRAM), the audio interface,
 * and the C library under libultra's names.
 *
 * The N64 structures are declared here as libultra lays them out (the games'
 * headers clash with the C library's).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "s30_audio.h"
#include "s30_gfx.h"
#include "s30_input.h"
#include "s30_os.h"

typedef void* OSMesg;
typedef struct OSMesgQueue {
    void* mtqueue;
    void* fullqueue;
    int32_t validCount;
    int32_t first;
    int32_t msgCount;
    OSMesg* msg;
} OSMesgQueue;

typedef struct {
    uint16_t button;
    int8_t stick_x;
    int8_t stick_y;
    uint8_t errnum;
} OSContPad;

typedef struct {
    uint16_t type;
    uint8_t status;
    uint8_t errnum;
} OSContStatus;

typedef struct {
    uint16_t type;
    uint8_t pri;
    uint8_t status;
    OSMesgQueue* retQueue;
} OSMesgHdr;

typedef struct {
    OSMesgHdr hdr;
    void* dramAddr;
    uint32_t devAddr;
    uint32_t size;
    void* piHandle;
} OSIoMesg;

typedef struct OSPiHandle {
    struct OSPiHandle* next;
    uint8_t type;
    uint8_t latency, pageSize, relDuration, pulse, domain;
    uint32_t baseAddress;
    uint32_t speed;
    uint8_t transferInfo[0x60];
} OSPiHandle;

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t* ucode_boot;
    uint32_t ucode_boot_size;
    uint64_t* ucode;
    uint32_t ucode_size;
    uint64_t* ucode_data;
    uint32_t ucode_data_size;
    uint64_t* dram_stack;
    uint32_t dram_stack_size;
    uint64_t* output_buff;
    uint64_t* output_buff_size;
    uint64_t* data_ptr;
    uint32_t data_size;
    uint64_t* yield_data_ptr;
    uint32_t yield_data_size;
} OSTask;

typedef uint64_t OSTime;

#define OS_MESG_BLOCK 1
#define OS_READ 0
#define OS_CPU_COUNTER 46875000ull
#define OS_EVENT_SP 4
#define OS_EVENT_SI 5
#define OS_EVENT_DP 9
#define OS_EVENT_COUNT 23
#define DEVICE_TYPE_SRAM 3
#define M_GFXTASK 1
#define PFS_ERR_NOPACK 1
#define CONT_NO_RESPONSE_ERROR 8

static S30OsConfig sConfig;

static uint64_t now_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

void S30Os_Init(const S30OsConfig* config) {
    sConfig = *config;
}

/* ---------------------------------------------------------------------------
 * Threads: one N64 thread on the CPU at a time
 * ------------------------------------------------------------------------- */

#define MAX_THREADS 256

typedef struct S30Thread {
    void* thread; // the game's OSThread
    int32_t id;
    int32_t pri;
    void (*entry)(void*);
    void* arg;
    int started;   // has a pthread
    int ready;     // wants the CPU (not blocked on a queue, not stopped)
    int stopped;   // osStopThread: until the next osStartThread
    int destroyed; // osDestroyThread: ends when it next waits for the CPU
    void* waitq;   // the queue it is blocked on
    pthread_t pt;
    pthread_cond_t cond;
} S30Thread;

static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER; // the CPU and every queue
static pthread_cond_t sQueueCond = PTHREAD_COND_INITIALIZER; // for threads not of the N64
static S30Thread* sThreads[MAX_THREADS];
static S30Thread* sRunning; // NULL: every N64 thread blocked (the N64 idles)
static __thread S30Thread* tSelf; // NULL on threads not of the N64

/* The highest-priority ready thread other than `except` */
static S30Thread* best_ready(S30Thread* except) {
    S30Thread* best = NULL;
    int i;

    for (i = 0; i < MAX_THREADS; i++) {
        S30Thread* t = sThreads[i];

        if (t != NULL && t->started && t->ready && t != except && (best == NULL || t->pri > best->pri)) {
            best = t;
        }
    }
    return best;
}

/* Gives the CPU to the best ready thread (lock held) */
static void dispatch(void) {
    sRunning = best_ready(NULL);
    if (sRunning != NULL) {
        pthread_cond_signal(&sRunning->cond);
    }
}

/* Waits until this thread holds the CPU (lock held). A destroyed thread ends
   here. */
static void wait_cpu(void) {
    while (sRunning != tSelf) {
        if (tSelf->destroyed) {
            S30Thread* t = tSelf;

            pthread_mutex_unlock(&sLock);
            pthread_cond_destroy(&t->cond);
            free(t);
            pthread_exit(NULL);
        }
        pthread_cond_wait(&tSelf->cond, &sLock);
    }
}

/* After making threads ready: yield if one outranks the running thread. From
   a thread not of the N64 (what the N64 did in an interrupt), the CPU goes to
   a thread that outranks the running one at once; the preempted thread runs
   on until its next OS call (cpu_check), where it waits for the CPU again.
   Games spin on flags that a higher-priority thread sets: on the N64 the
   interrupt preempted them. */
static void preempt(void) {
    S30Thread* b;

    if (tSelf == NULL) {
        b = best_ready(NULL);
        if (sRunning == NULL || (b != NULL && b->pri > sRunning->pri)) {
            dispatch();
        }
        return;
    }
    if (sRunning == tSelf) {
        b = best_ready(tSelf);
        if (b != NULL && b->pri > tSelf->pri) {
            dispatch();
            wait_cpu();
        }
    }
}

/* An N64 thread entering the OS: if it was preempted, it waits for the CPU
   here (lock held) */
static void cpu_check(void) {
    if (tSelf != NULL && sRunning != tSelf) {
        wait_cpu();
    }
}

static int find_slot(void* thread) {
    int i;

    for (i = 0; i < MAX_THREADS; i++) {
        if (sThreads[i] != NULL && sThreads[i]->thread == thread) {
            return i;
        }
    }
    return -1;
}

static S30Thread* find_thread(void* thread) {
    int i;

    if (thread == NULL) {
        return tSelf;
    }
    i = find_slot(thread);
    return i >= 0 ? sThreads[i] : NULL;
}

/* Takes a thread out of the table (lock held). One with a pthread ends when
   it next waits for the CPU; it frees itself. */
static void retire(int slot) {
    S30Thread* t = sThreads[slot];

    sThreads[slot] = NULL;
    if (t->started) {
        t->destroyed = 1;
        t->ready = 0;
        pthread_cond_signal(&t->cond);
    } else {
        pthread_cond_destroy(&t->cond);
        free(t);
    }
}

void osCreateThread(void* thread, int32_t id, void (*entry)(void*), void* arg, void* sp, int32_t pri) {
    S30Thread* t;
    int i;

    if (!sConfig.threads) {
        return;
    }
    pthread_mutex_lock(&sLock);
    i = find_slot(thread);
    if (i >= 0) {
        retire(i); // the OSThread is reused
    }
    for (i = 0; i < MAX_THREADS && sThreads[i] != NULL; i++) {
    }
    if (i == MAX_THREADS) {
        pthread_mutex_unlock(&sLock);
        fprintf(stderr, "os: more than %d threads\n", MAX_THREADS);
        abort();
    }
    t = calloc(1, sizeof(*t));
    t->thread = thread;
    t->id = id;
    t->pri = pri;
    t->entry = entry;
    t->arg = arg;
    pthread_cond_init(&t->cond, NULL);
    sThreads[i] = t;
    pthread_mutex_unlock(&sLock);
}

void S30App_SignalStack(void); // app.c

static void* thread_main(void* p) {
    S30Thread* t = p;
    char name[16];
    int i;

    tSelf = t;
    snprintf(name, sizeof(name), "n64-thread%d", (int)t->id);
    pthread_setname_np(pthread_self(), name);
    S30App_SignalStack();
    pthread_mutex_lock(&sLock);
    wait_cpu();
    pthread_mutex_unlock(&sLock);
    t->entry(t->arg);
    // The thread returned: off the CPU for good
    pthread_mutex_lock(&sLock);
    for (i = 0; i < MAX_THREADS; i++) {
        if (sThreads[i] == t) {
            sThreads[i] = NULL;
        }
    }
    if (sRunning == t) {
        t->ready = 0;
        dispatch();
    }
    pthread_mutex_unlock(&sLock);
    pthread_cond_destroy(&t->cond);
    free(t);
    return NULL;
}

void osStartThread(void* thread) {
    S30Thread* t;

    if (!sConfig.threads) {
        return;
    }
    pthread_mutex_lock(&sLock);
    cpu_check();
    t = find_thread(thread);
    if (t != NULL && !t->started) {
        pthread_attr_t attr;

        t->started = 1;
        t->ready = 1;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 256 << 10);
        pthread_create(&t->pt, &attr, thread_main, t);
        pthread_attr_destroy(&attr);
        preempt();
    } else if (t != NULL && t->stopped) {
        t->stopped = 0;
        t->ready = t->waitq == NULL;
        preempt();
    }
    pthread_mutex_unlock(&sLock);
}

/* Stops a thread until osStartThread; the running thread gives up the CPU */
void osStopThread(void* thread) {
    S30Thread* t;

    if (!sConfig.threads) {
        return;
    }
    pthread_mutex_lock(&sLock);
    cpu_check();
    t = find_thread(thread);
    if (t != NULL) {
        t->stopped = 1;
        t->ready = 0;
        if (t == tSelf) {
            dispatch();
            wait_cpu();
        }
    }
    pthread_mutex_unlock(&sLock);
}

void osDestroyThread(void* thread) {
    S30Thread* t;
    int i;

    if (!sConfig.threads) {
        return;
    }
    pthread_mutex_lock(&sLock);
    cpu_check();
    t = find_thread(thread);
    if (t == tSelf && t != NULL) {
        // Ends the calling thread
        for (i = 0; i < MAX_THREADS; i++) {
            if (sThreads[i] == t) {
                sThreads[i] = NULL;
            }
        }
        t->ready = 0;
        t->destroyed = 1;
        dispatch();
        wait_cpu(); // exits
    }
    i = find_slot(thread);
    if (i >= 0) {
        retire(i);
    }
    pthread_mutex_unlock(&sLock);
}

/* Gives the CPU to another ready thread of the same priority */
void osYieldThread(void) {
    S30Thread* b;

    if (tSelf == NULL) {
        return;
    }
    pthread_mutex_lock(&sLock);
    cpu_check();
    b = best_ready(tSelf);
    if (b != NULL && b->pri >= tSelf->pri) {
        sRunning = b;
        pthread_cond_signal(&b->cond);
        wait_cpu();
    }
    pthread_mutex_unlock(&sLock);
}

void osSetThreadPri(void* thread, int32_t pri) {
    S30Thread* t;

    pthread_mutex_lock(&sLock);
    cpu_check();
    t = find_thread(thread);
    if (t != NULL) {
        t->pri = pri;
        preempt();
    }
    pthread_mutex_unlock(&sLock);
}

int32_t osGetThreadPri(void* thread) {
    S30Thread* t;
    int32_t pri;

    pthread_mutex_lock(&sLock);
    t = find_thread(thread);
    pri = t != NULL ? t->pri : 0;
    pthread_mutex_unlock(&sLock);
    return pri;
}

int32_t osGetThreadId(void* thread) {
    S30Thread* t;
    int32_t id;

    pthread_mutex_lock(&sLock);
    t = find_thread(thread);
    id = t != NULL ? t->id : 0;
    pthread_mutex_unlock(&sLock);
    return id;
}

/* No interrupts to mask: a thread keeps the CPU until it blocks anyway */
uint32_t osSetIntMask(uint32_t mask) {
    return 0;
}

/* ---------------------------------------------------------------------------
 * Message queues and events
 * ------------------------------------------------------------------------- */

void osCreateMesgQueue(OSMesgQueue* mq, OSMesg* msg, int32_t count) {
    mq->mtqueue = mq->fullqueue = NULL;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msg;
}

/* Blocks the calling thread until `mq` changes (lock held). An N64 thread
   gives the CPU to the next one meanwhile. */
static void block(OSMesgQueue* mq) {
    if (tSelf == NULL) {
        pthread_cond_wait(&sQueueCond, &sLock);
        return;
    }
    tSelf->waitq = mq;
    tSelf->ready = 0;
    dispatch();
    wait_cpu();
}

/* The threads blocked on `mq` are ready again; one that outranks the running
   thread takes the CPU (lock held) */
static void queue_changed(OSMesgQueue* mq) {
    int i;

    for (i = 0; i < MAX_THREADS; i++) {
        S30Thread* t = sThreads[i];

        if (t != NULL && t->waitq == mq) {
            t->waitq = NULL;
            t->ready = !t->stopped;
        }
    }
    pthread_cond_broadcast(&sQueueCond);
    preempt();
}

static int32_t send_mesg(OSMesgQueue* mq, OSMesg msg, int32_t flag, int jam) {
    if (mq == NULL && sConfig.noWait) {
        return -1;
    }
    pthread_mutex_lock(&sLock);
    cpu_check();
    while (mq->validCount >= mq->msgCount) {
        if (flag != OS_MESG_BLOCK || sConfig.noWait) {
            pthread_mutex_unlock(&sLock);
            return -1;
        }
        block(mq);
    }
    if (jam) {
        mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
        mq->msg[mq->first] = msg;
    } else {
        mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
    }
    mq->validCount++;
    queue_changed(mq);
    pthread_mutex_unlock(&sLock);
    return 0;
}

int32_t osSendMesg(OSMesgQueue* mq, OSMesg msg, int32_t flag) {
    return send_mesg(mq, msg, flag, 0);
}

int32_t osJamMesg(OSMesgQueue* mq, OSMesg msg, int32_t flag) {
    return send_mesg(mq, msg, flag, 1);
}

int32_t osRecvMesg(OSMesgQueue* mq, OSMesg* msg, int32_t flag) {
    if (mq == NULL && sConfig.noWait) {
        return -1;
    }
    pthread_mutex_lock(&sLock);
    cpu_check();
    while (mq->validCount == 0) {
        if (flag != OS_MESG_BLOCK || sConfig.noWait) {
            pthread_mutex_unlock(&sLock);
            return -1;
        }
        block(mq);
    }
    if (msg != NULL) {
        *msg = mq->msg[mq->first];
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    queue_changed(mq);
    pthread_mutex_unlock(&sLock);
    return 0;
}

static struct {
    OSMesgQueue* mq;
    OSMesg msg;
} sEvents[OS_EVENT_COUNT];

void osSetEventMesg(int32_t event, OSMesgQueue* mq, OSMesg msg) {
    if (event >= 0 && event < OS_EVENT_COUNT) {
        sEvents[event].mq = mq;
        sEvents[event].msg = msg;
    }
}

static void post_event(int32_t event) {
    if (sEvents[event].mq != NULL) {
        osSendMesg(sEvents[event].mq, sEvents[event].msg, 0);
    }
}

void* __osGetActiveQueue(void) {
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Time and timers (in N64 CPU cycles, 46.875 MHz)
 * ------------------------------------------------------------------------- */

uint64_t osClockRate = 62500000;
static uint64_t sTimeBase; // ns
static OSTime sTimeOffset; // osSetTime

OSTime osGetTime(void) {
    uint64_t ns = now_ns() - sTimeBase;

    return sTimeOffset + ns / 1000 * OS_CPU_COUNTER / 1000000;
}

void osSetTime(OSTime t) {
    sTimeOffset = t - (osGetTime() - sTimeOffset);
}

uint32_t osGetCount(void) {
    return (uint32_t)osGetTime();
}

typedef struct OSTimer {
    struct OSTimer* next;
    struct OSTimer* prev;
    OSTime interval;
    OSTime value;
    OSMesgQueue* mq;
    OSMesg msg;
} OSTimer;

#define MAX_TIMERS 32
static struct {
    OSTimer* timer;
    uint64_t due;      // ns
    uint64_t interval; // ns, 0 for one-shot
    OSMesgQueue* mq;
    OSMesg msg;
} sTimers[MAX_TIMERS];
static pthread_mutex_t sTimerLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sTimerCond;
static int sTimerThreadStarted;

static uint64_t cycles_to_ns(OSTime c) {
    return c * 1000ull / (OS_CPU_COUNTER / 1000000ull);
}

static void* timer_thread(void* arg) {
    pthread_setname_np(pthread_self(), "s30-timers");
    pthread_mutex_lock(&sTimerLock);
    for (;;) {
        uint64_t now = now_ns(), next = UINT64_MAX;
        int i;

        for (i = 0; i < MAX_TIMERS; i++) {
            if (sTimers[i].timer == NULL) {
                continue;
            }
            if (sTimers[i].due <= now) {
                OSMesgQueue* mq = sTimers[i].mq;
                OSMesg msg = sTimers[i].msg;

                if (sTimers[i].interval != 0) {
                    sTimers[i].due += sTimers[i].interval;
                } else {
                    sTimers[i].timer = NULL;
                }
                pthread_mutex_unlock(&sTimerLock);
                if (mq != NULL) {
                    osSendMesg(mq, msg, 0);
                }
                pthread_mutex_lock(&sTimerLock);
                i = -1; // rescan: the list may have changed
                now = now_ns();
                next = UINT64_MAX;
                continue;
            }
            if (sTimers[i].due < next) {
                next = sTimers[i].due;
            }
        }
        if (next == UINT64_MAX) {
            pthread_cond_wait(&sTimerCond, &sTimerLock);
        } else {
            struct timespec ts;

            ts.tv_sec = next / 1000000000ull;
            ts.tv_nsec = next % 1000000000ull;
            pthread_cond_timedwait(&sTimerCond, &sTimerLock, &ts);
        }
    }
    return NULL;
}

int32_t osSetTimer(OSTimer* timer, OSTime countdown, OSTime interval, OSMesgQueue* mq, OSMesg msg) {
    int i;

    pthread_mutex_lock(&sTimerLock);
    if (!sTimerThreadStarted) {
        pthread_t t;
        pthread_condattr_t ca;

        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&sTimerCond, &ca);
        pthread_create(&t, NULL, timer_thread, NULL);
        pthread_detach(t);
        sTimerThreadStarted = 1;
    }
    for (i = 0; i < MAX_TIMERS; i++) {
        if (sTimers[i].timer == NULL || sTimers[i].timer == timer) {
            sTimers[i].timer = timer;
            sTimers[i].due = now_ns() + cycles_to_ns(countdown != 0 ? countdown : interval);
            sTimers[i].interval = cycles_to_ns(interval);
            sTimers[i].mq = mq;
            sTimers[i].msg = msg;
            break;
        }
    }
    if (i == MAX_TIMERS) {
        fprintf(stderr, "os: more than %d timers\n", MAX_TIMERS);
    }
    pthread_cond_broadcast(&sTimerCond);
    pthread_mutex_unlock(&sTimerLock);
    return 0;
}

int32_t osStopTimer(OSTimer* timer) {
    int i;

    pthread_mutex_lock(&sTimerLock);
    for (i = 0; i < MAX_TIMERS; i++) {
        if (sTimers[i].timer == timer) {
            sTimers[i].timer = NULL;
        }
    }
    pthread_mutex_unlock(&sTimerLock);
    return 0;
}

/* ---------------------------------------------------------------------------
 * VI: retrace messages at 60 Hz (once osCreateViManager has run), the
 * framebuffer pointers tracked
 * ------------------------------------------------------------------------- */

static OSMesgQueue* sViQueue;
static OSMesg sViMsg;
static void* sViCurrent;
static void* sViNext;
static int sGfxTasks, sAudioTasks; // <PREFIX>_STATS

void osViSetEvent(OSMesgQueue* mq, OSMesg msg, uint32_t retraceCount) {
    sViQueue = mq;
    sViMsg = msg;
}

void* osViGetCurrentFramebuffer(void) {
    return sViCurrent;
}

void* osViGetNextFramebuffer(void) {
    return sViNext;
}

void osViSwapBuffer(void* frameBufPtr) {
    sViNext = frameBufPtr;
}

static void* vi_thread(void* arg) {
    uint64_t next = now_ns();
    int retraces = 0;
    int stats = S30_Getenv("STATS") != NULL;

    pthread_setname_np(pthread_self(), "s30-vi");
    for (;;) {
        struct timespec ts;

        next += 1000000000ull / 60;
        ts.tv_sec = next / 1000000000ull;
        ts.tv_nsec = next % 1000000000ull;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        sViCurrent = sViNext;
        if (sViQueue != NULL) {
            osSendMesg(sViQueue, sViMsg, 0);
        }
        if (stats && ++retraces % 120 == 0) {
            S30Thread* r = sRunning;

            fprintf(stderr, "os: %d gfx tasks, %d audio tasks in 2 s; thread %d on the CPU\n", sGfxTasks,
                    sAudioTasks, r != NULL ? (int)r->id : -1);
            sGfxTasks = sAudioTasks = 0;
        }
    }
    return NULL;
}

void osCreateViManager(int32_t pri) {
    static int started;
    pthread_t t;

    if (sConfig.vi && !started) {
        started = 1;
        pthread_create(&t, NULL, vi_thread, NULL);
        pthread_detach(t);
    }
}

void osViSetMode(void* mode) {
}
void osViBlack(uint8_t active) {
}
void osViSetYScale(float scale) {
}
void osViSetXScale(float scale) {
}
void osViSetSpecialFeatures(uint32_t func) {
}
void osViRepeatLine(uint8_t active) {
}

/* ---------------------------------------------------------------------------
 * RCP: tasks run on the main thread (S30Os_RunRcp). Without it (ports that
 * draw from their own loop) starting a task does nothing.
 * ------------------------------------------------------------------------- */

#define TASK_QUEUE 8
static OSTask* sTasks[TASK_QUEUE];
static int sTaskHead, sTaskCount;
static int sRcpRunning;
static pthread_mutex_t sTaskLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sTaskCond = PTHREAD_COND_INITIALIZER;

void osSpTaskLoad(OSTask* task) {
}

void osSpTaskStartGo(OSTask* task) {
    if (!sRcpRunning) {
        return;
    }
    pthread_mutex_lock(&sTaskLock);
    if (sTaskCount == TASK_QUEUE) {
        fprintf(stderr, "os: RCP task queue full\n");
    } else {
        sTasks[(sTaskHead + sTaskCount++) % TASK_QUEUE] = task;
        pthread_cond_signal(&sTaskCond);
    }
    pthread_mutex_unlock(&sTaskLock);
}

/* Tasks run to the end: a yield never happens */
void osSpTaskYield(void) {
}

uint32_t osSpTaskYielded(OSTask* task) {
    return 0;
}

void osDpSetNextBuffer(void* buf, uint64_t size) {
}
void osDpSetStatus(uint32_t data) {
}
uint32_t osDpGetStatus(void) {
    return 0;
}
uint32_t __osSpGetStatus(void) {
    return 0;
}
void __osSpSetStatus(uint32_t data) {
}

void S30Os_RunRcp(void) {
    sRcpRunning = 1;
    for (;;) {
        OSTask* task;

        pthread_mutex_lock(&sTaskLock);
        while (sTaskCount == 0) {
            pthread_cond_wait(&sTaskCond, &sTaskLock);
        }
        task = sTasks[sTaskHead];
        sTaskHead = (sTaskHead + 1) % TASK_QUEUE;
        sTaskCount--;
        pthread_mutex_unlock(&sTaskLock);

        if (task->type == M_GFXTASK) {
            sGfxTasks++;
            pc_process_displaylist(task->data_ptr);
            post_event(OS_EVENT_SP);
            post_event(OS_EVENT_DP);
        } else {
            sAudioTasks++;
            post_event(OS_EVENT_SP);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Controllers: the S30 gamepad as controller 1; no Controller or Rumble Pak
 * ------------------------------------------------------------------------- */

int32_t osContInit(OSMesgQueue* mq, uint8_t* bitpattern, OSContStatus* status) {
    int i;

    for (i = 0; i < 4; i++) {
        status[i].type = 0x0500; // CONT_TYPE_NORMAL
        status[i].status = 0;
        status[i].errnum = i == 0 ? 0 : CONT_NO_RESPONSE_ERROR;
    }
    *bitpattern = 1;
    return 0;
}

int32_t osContStartReadData(OSMesgQueue* mq) {
    osSendMesg(mq, NULL, 0);
    return 0;
}

void osContGetReadData(OSContPad* pad) {
    S30Pad p;
    int i;

    S30Input_Read(&p);
    memset(pad, 0, 4 * sizeof(*pad));
    pad[0].button = p.button;
    pad[0].stick_x = p.stick_x;
    pad[0].stick_y = p.stick_y;
    for (i = 1; i < 4; i++) {
        pad[i].errnum = CONT_NO_RESPONSE_ERROR;
    }
}

int32_t osContStartQuery(OSMesgQueue* mq) {
    osSendMesg(mq, NULL, 0);
    return 0;
}

void osContGetQuery(OSContStatus* status) {
    uint8_t bits;

    osContInit(NULL, &bits, status);
}

int32_t osMotorInit(OSMesgQueue* mq, void* pfs, int32_t channel) {
    return PFS_ERR_NOPACK;
}
int32_t osMotorStart(void* pfs) {
    return PFS_ERR_NOPACK;
}
int32_t osMotorStop(void* pfs) {
    return PFS_ERR_NOPACK;
}
int32_t __osMotorAccess(void* pfs, int32_t flag) {
    return PFS_ERR_NOPACK;
}

/* The Controller Pak through the file system's API, for ports that don't build
   libultra's (weak): no pak. A port that builds it gets the pak below (PIF). */
__attribute__((weak)) int32_t osPfsIsPlug(OSMesgQueue* mq, uint8_t* pattern) {
    *pattern = 0;
    return 0;
}
__attribute__((weak)) int32_t osPfsInit(OSMesgQueue* mq, void* pfs, int32_t channel) {
    return PFS_ERR_NOPACK;
}
__attribute__((weak)) int32_t osPfsInitPak(OSMesgQueue* mq, void* pfs, int32_t channel) {
    return PFS_ERR_NOPACK;
}
__attribute__((weak)) int32_t osPfsFindFile(void* pfs, uint16_t company, uint32_t game, uint8_t* name, uint8_t* ext, int32_t* file) {
    return PFS_ERR_NOPACK;
}
__attribute__((weak)) int32_t osPfsDeleteFile(void* pfs, uint16_t company, uint32_t game, uint8_t* name, uint8_t* ext) {
    return PFS_ERR_NOPACK;
}
__attribute__((weak)) int32_t osPfsAllocateFile(void* pfs, uint16_t company, uint32_t game, uint8_t* name, uint8_t* ext, int32_t size,
                          int32_t* file) {
    return PFS_ERR_NOPACK;
}
__attribute__((weak)) int32_t osPfsReadWriteFile(void* pfs, int32_t file, uint8_t flag, int32_t offset, int32_t size, uint8_t* data) {
    return PFS_ERR_NOPACK;
}
__attribute__((weak)) int32_t osPfsFileState(void* pfs, int32_t file, void* state) {
    return PFS_ERR_NOPACK;
}
__attribute__((weak)) int32_t osPfsNumFiles(void* pfs, int32_t* maxFiles, int32_t* filesUsed) {
    return PFS_ERR_NOPACK;
}
__attribute__((weak)) int32_t osPfsFreeBlocks(void* pfs, int32_t* bytesNotUsed) {
    return PFS_ERR_NOPACK;
}
__attribute__((weak)) int32_t osPfsChecker(void* pfs) {
    return PFS_ERR_NOPACK;
}
__attribute__((weak)) int32_t osPfsRepairId(void* pfs) {
    return PFS_ERR_NOPACK;
}

/* The Voice Recognition Unit: none plugged in */
int32_t osVoiceInit(OSMesgQueue* mq, void* hd, int32_t channel) {
    return PFS_ERR_NOPACK;
}
int32_t osVoiceGetStatus(OSMesgQueue* mq, int32_t channel, uint8_t* status) {
    *status = 0;
    return PFS_ERR_NOPACK;
}
int32_t osVoiceSetWord(void* hd, uint8_t* word) {
    return PFS_ERR_NOPACK;
}
int32_t osVoiceCheckWord(uint8_t* word) {
    return 0;
}
int32_t osVoiceStartReadData(void* hd) {
    return PFS_ERR_NOPACK;
}
int32_t osVoiceStopReadData(void* hd) {
    return PFS_ERR_NOPACK;
}
int32_t osVoiceGetReadData(void* hd, void* result) {
    return PFS_ERR_NOPACK;
}
int32_t osVoiceClearDictionary(void* hd, uint8_t numWords) {
    return PFS_ERR_NOPACK;
}
int32_t osVoiceMaskDictionary(void* hd, uint8_t* maskPattern, int32_t size) {
    return PFS_ERR_NOPACK;
}
int32_t osVoiceControlGain(void* hd, int32_t analog, int32_t digital) {
    return PFS_ERR_NOPACK;
}

/* ---------------------------------------------------------------------------
 * Saves: EEPROM (blocks of 8 bytes) or SRAM, in sConfig.savePath
 * ------------------------------------------------------------------------- */

static uint8_t* sSave;
static size_t sSaveSize;
static int32_t write_file(const char* path, const void* data, size_t size);
static void read_file(const char* path, void* data, size_t size);

static void save_load(void) {
    if (sSave != NULL) {
        return;
    }
    switch (sConfig.saveType) {
        case S30_SAVE_EEPROM_4K:
            sSaveSize = 512;
            break;
        case S30_SAVE_EEPROM_16K:
            sSaveSize = 2048;
            break;
        case S30_SAVE_SRAM:
            sSaveSize = 0x8000;
            break;
        default:
            sSaveSize = 1;
            break;
    }
    sSave = malloc(sSaveSize);
    read_file(sConfig.savePath, sSave, sSaveSize);
}

/* A save file written whole, through a temporary file made durable before it
   replaces the old one: a power cut leaves the previous save */
static int32_t write_file(const char* path, const void* data, size_t size) {
    char tmp[4096];
    int fd;

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0 || write(fd, data, size) != (ssize_t)size || fsync(fd) != 0) {
        perror(tmp);
        if (fd >= 0) {
            close(fd);
            unlink(tmp);
        }
        return -1;
    }
    close(fd);
    if (rename(tmp, path) != 0) {
        perror(path);
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* A save file's contents into `data` (zeros where it is missing or short) */
static void read_file(const char* path, void* data, size_t size) {
    FILE* f = path != NULL ? fopen(path, "rb") : NULL;

    memset(data, 0, size);
    if (f != NULL) {
        if (fread(data, 1, size, f) != size) {
            fprintf(stderr, "os: %s: short file\n", path);
        }
        fclose(f);
    }
}

static int32_t save_write(void) {
    return sConfig.savePath != NULL ? write_file(sConfig.savePath, sSave, sSaveSize) : -1;
}

/* Writes `size` bytes of the save at `offset`; the file only if they changed */
static int32_t save_store(uint32_t offset, const void* data, uint32_t size) {
    if (memcmp(&sSave[offset], data, size) == 0) {
        return 0;
    }
    memcpy(&sSave[offset], data, size);
    return save_write();
}

int32_t osEepromProbe(OSMesgQueue* mq) {
    return sConfig.saveType == S30_SAVE_EEPROM_4K ? 1 : sConfig.saveType == S30_SAVE_EEPROM_16K ? 2 : 0;
}

int32_t osEepromLongRead(OSMesgQueue* mq, uint8_t address, uint8_t* buffer, int32_t nbytes) {
    save_load();
    if (address * 8 + nbytes > (int32_t)sSaveSize) {
        return -1;
    }
    memcpy(buffer, &sSave[address * 8], nbytes);
    return 0;
}

int32_t osEepromLongWrite(OSMesgQueue* mq, uint8_t address, uint8_t* buffer, int32_t nbytes) {
    save_load();
    if (address * 8 + nbytes > (int32_t)sSaveSize) {
        return -1;
    }
    return save_store(address * 8, buffer, nbytes);
}

int32_t osEepromRead(OSMesgQueue* mq, uint8_t address, uint8_t* buffer) {
    return osEepromLongRead(mq, address, buffer, 8);
}

int32_t osEepromWrite(OSMesgQueue* mq, uint8_t address, uint8_t* buffer) {
    return osEepromLongWrite(mq, address, buffer, 8);
}

static void sram_access(uint32_t addr, void* ram, uint32_t size, int32_t direction) {
    save_load();
    addr &= sSaveSize - 1;
    if (addr + size > sSaveSize) {
        size = sSaveSize - addr;
    }
    if (direction == OS_READ) {
        memcpy(ram, &sSave[addr], size);
    } else {
        save_store(addr, ram, size);
    }
}

/* ---------------------------------------------------------------------------
 * The PIF, for libultra's own Controller Pak code (osPfs*, __osContRamRead,
 * __osContRamWrite): the commands it puts in the PIF RAM are answered here.
 * Controller 1 has a Controller Pak if sConfig.pakPath is set: a 32 KB image,
 * written whole when a write changes it. A blank image is all zeros; the
 * game's libultra formats it (osPfsInit: a new ID, then osPfsChecker).
 * ------------------------------------------------------------------------- */

#define PIF_RAM_SIZE 64
#define PAK_SIZE 0x8000
#define CONT_CMD_REQUEST_STATUS 0x00
#define CONT_CMD_READ_MEMPAK 0x02
#define CONT_CMD_WRITE_MEMPAK 0x03
#define CONT_CMD_RESET 0xFF
#define CHNL_ERR_NORESP 0x80

static uint8_t sPifRam[PIF_RAM_SIZE];
static uint8_t* sPak;
static pthread_mutex_t sPifLock = PTHREAD_MUTEX_INITIALIZER;

// libultra's PIF state, for the ports that don't build its controller code
__attribute__((weak)) uint8_t __osContLastCmd;
__attribute__((weak)) uint8_t __osMaxControllers = 4;
__attribute__((weak)) uint8_t _osContNumControllers = 4; // its name in some decomps

/* libultra's data CRC of a 32-byte block (__osContDataCrc) */
static uint8_t pak_crc(const uint8_t* data) {
    uint8_t temp = 0;
    int i, j;

    for (i = 0; i <= 32; i++) {
        for (j = 7; j >= 0; j--) {
            uint8_t xor = (temp & 0x80) ? 0x85 : 0;

            temp <<= 1;
            if (i < 32) {
                temp |= (data[i] >> j) & 1;
            }
            temp ^= xor;
        }
    }
    return temp;
}

/* A pak command's address: libultra's formats declare it a u16 (block << 5 |
   address CRC), so the game, built little-endian, stores it low byte first */
static uint16_t pak_address(const uint8_t* cmd) {
    return (uint16_t)(cmd[1] | (cmd[2] << 8)) & ~0x1F;
}

/* One command to controller `channel`: `cmd` (tx bytes), answer in `res` (rx
   bytes). Returns 0, or CHNL_ERR_NORESP for nothing connected. */
static uint8_t pif_command(int channel, const uint8_t* cmd, int tx, uint8_t* res, int rx) {
    uint16_t addr;

    if (channel != 0 || tx < 1) {
        return CHNL_ERR_NORESP; // controllers 2 to 4
    }
    switch (cmd[0]) {
        case CONT_CMD_REQUEST_STATUS:
        case CONT_CMD_RESET:
            if (rx >= 3) {
                res[0] = 0x05; // CONT_TYPE_NORMAL
                res[1] = 0x00;
                res[2] = sConfig.pakPath != NULL ? 0x01 : 0x00; // CONT_CARD_ON
            }
            return 0;
        case CONT_CMD_READ_MEMPAK:
            if (sConfig.pakPath == NULL || tx < 3 || rx < 33) {
                return CHNL_ERR_NORESP;
            }
            addr = pak_address(cmd);
            if (addr < PAK_SIZE) {
                memcpy(res, &sPak[addr], 32);
            } else {
                memset(res, 0, 32); // bank select, Rumble Pak probe: a 1-bank pak
            }
            res[32] = pak_crc(res);
            return 0;
        case CONT_CMD_WRITE_MEMPAK:
            if (sConfig.pakPath == NULL || tx < 35 || rx < 1) {
                return CHNL_ERR_NORESP;
            }
            addr = pak_address(cmd);
            if (addr < PAK_SIZE && memcmp(&sPak[addr], &cmd[3], 32) != 0) {
                memcpy(&sPak[addr], &cmd[3], 32);
                write_file(sConfig.pakPath, sPak, PAK_SIZE);
            }
            res[0] = pak_crc(&cmd[3]);
            return 0;
        default:
            return CHNL_ERR_NORESP;
    }
}

/* The PIF RAM's commands, as the PIF runs them: per channel a tx size, an rx
   size, the command and room for the answer; 0x00 skips a channel, 0xFF is
   padding, 0xFE ends the list */
static void pif_run(void) {
    int i = 0, channel = 0;

    while (i < PIF_RAM_SIZE - 1) {
        uint8_t tx = sPifRam[i];
        int txn, rxn;

        if (tx == 0xFE) {
            break;
        }
        if (tx == 0xFF || tx == 0xFD) {
            i++;
            continue;
        }
        if (tx == 0x00) {
            channel++;
            i++;
            continue;
        }
        if (i + 1 >= PIF_RAM_SIZE - 1 || sPifRam[i + 1] == 0xFE) {
            break;
        }
        txn = tx & 0x3F;
        rxn = sPifRam[i + 1] & 0x3F;
        if (i + 2 + txn + rxn > PIF_RAM_SIZE - 1) {
            break;
        }
        sPifRam[i + 1] = (sPifRam[i + 1] & 0x3F) |
                         pif_command(channel, &sPifRam[i + 2], txn, &sPifRam[i + 2 + txn], rxn);
        i += 2 + txn + rxn;
        channel++;
    }
}

int32_t __osSiGetAccess(void) {
    return 0;
}
void __osSiRelAccess(void) {
}
int32_t __osSiDeviceBusy(void) {
    return 0;
}

/* A write runs the commands; a read gives the answers. Either one signals the
   SI event, as its interrupt did. */
int32_t __osSiRawStartDma(int32_t direction, void* dramAddr) {
    pthread_mutex_lock(&sPifLock);
    if (sPak == NULL && sConfig.pakPath != NULL) {
        sPak = malloc(PAK_SIZE);
        read_file(sConfig.pakPath, sPak, PAK_SIZE);
    }
    if (direction == OS_READ) {
        memcpy(dramAddr, sPifRam, PIF_RAM_SIZE);
    } else {
        memcpy(sPifRam, dramAddr, PIF_RAM_SIZE);
        pif_run();
    }
    pthread_mutex_unlock(&sPifLock);
    post_event(OS_EVENT_SI);
    return 0;
}

/* ---------------------------------------------------------------------------
 * PI DMA: the ROM image (reads past its end give zeros) and SRAM
 * ------------------------------------------------------------------------- */

static int sRomFd = -1;

static void rom_read(uint32_t devAddr, void* dst, uint32_t size) {
    ssize_t n;

    if (sConfig.beforeDma != NULL) {
        sConfig.beforeDma(dst, size);
    }
    if (sRomFd < 0) {
        if (sConfig.romPath == NULL) {
            fprintf(stderr, "os: PI DMA from %08x, but no ROM image\n", devAddr);
            abort();
        }
        sRomFd = open(sConfig.romPath, O_RDONLY | O_CLOEXEC);
        if (sRomFd < 0) {
            perror(sConfig.romPath);
            exit(1);
        }
    }
    n = pread(sRomFd, dst, size, devAddr & 0x0FFFFFFF);
    if (n != (ssize_t)size) {
        memset((uint8_t*)dst + (n > 0 ? n : 0), 0, size - (n > 0 ? n : 0));
    }
}

OSPiHandle* osCartRomInit(void) {
    static OSPiHandle handle;

    handle.type = 0; // DEVICE_TYPE_CART
    handle.baseAddress = 0xB0000000;
    return &handle;
}

OSPiHandle* osDriveRomInit(void) {
    return osCartRomInit();
}

/* The PI handles the game made (osEPiLinkHandle; some games link their own) */
OSPiHandle* __osPiTable;

int32_t osEPiLinkHandle(OSPiHandle* handle) {
    handle->next = __osPiTable;
    __osPiTable = handle;
    return 0;
}

int32_t osPiStartDma(OSIoMesg* mb, int32_t pri, int32_t direction, uint32_t devAddr, void* vAddr, uint32_t nbytes,
                     OSMesgQueue* mq) {
    if (sConfig.romPath == NULL) {
        return -1; // no ROM image: as with no PI manager running
    }
    if (direction == OS_READ) {
        rom_read(devAddr, vAddr, nbytes);
    }
    if (mq != NULL) {
        osSendMesg(mq, mb, 0);
    }
    return 0;
}

int32_t osEPiStartDma(OSPiHandle* handle, OSIoMesg* mb, int32_t direction) {
    if (handle != NULL && handle->type == DEVICE_TYPE_SRAM) {
        sram_access(mb->devAddr, mb->dramAddr, mb->size, direction);
    } else if (sConfig.romPath == NULL) {
        return -1; // no ROM image: as with no PI manager running
    } else if (direction == OS_READ) {
        rom_read(mb->devAddr, mb->dramAddr, mb->size);
    }
    if (mb->hdr.retQueue != NULL) {
        osSendMesg(mb->hdr.retQueue, mb, 0);
    }
    return 0;
}

void osCreatePiManager(int32_t pri, OSMesgQueue* cmdQ, OSMesg* cmdBuf, int32_t cmdMsgCnt) {
}

/* ---------------------------------------------------------------------------
 * Audio interface: buffers go to ALSA (s30_audio.h)
 * ------------------------------------------------------------------------- */

__attribute__((weak)) int32_t osAiSetFrequency(uint32_t frequency) {
    return frequency;
}

__attribute__((weak)) uint32_t osAiGetLength(void) {
    return sConfig.aiLength ? S30Audio_QueuedFrames() * 4 : 0;
}

/* Weak, as osAiSetFrequency and osAiGetLength: OoT and MM have their own */
__attribute__((weak)) int32_t osAiSetNextBuffer(void* buf, uint32_t size) {
    static int dbg = -1, count, peak;
    static FILE* dump;

    if (dbg < 0) {
        dbg = S30_Getenv("AUDIODBG") != NULL;
        if (S30_Getenv("AUDIODUMP") != NULL) {
            dump = fopen(S30_TmpPath("audio.raw"), "wb");
        }
    }
    if (dbg) { // the loudest sample, the buffer and the queue, once a second
        const int16_t* p = buf;
        uint32_t i;

        for (i = 0; i < size / 2; i++) {
            int v = p[i] < 0 ? -p[i] : p[i];
            peak = v > peak ? v : peak;
        }
        if (++count % 60 == 0) {
            printf("audio: peak %d, buffer %u frames, queued %u, underruns %d\n", peak, (unsigned)(size / 4),
                   S30Audio_QueuedFrames(), S30Audio_Underruns());
            if (sConfig.audioDebug != NULL) {
                sConfig.audioDebug();
            }
            peak = 0;
        }
    }
    if (dump != NULL) {
        fwrite(buf, 1, size, dump);
    }
    S30Audio_PlayInterleaved(buf, size / 4);
    return 0;
}

/* ---------------------------------------------------------------------------
 * The rest of the N64: nothing to do
 * ------------------------------------------------------------------------- */

int32_t osTvType = 1;    // OS_TV_NTSC
int32_t osResetType = 0; // cold reset

void osInitialize(void) {
    sTimeBase = now_ns();
}

/* Timers start with the first osSetTimer */
void __osTimerServicesInit(void) {
}

/* No CPU faults reach the game (app.c reports crashes) */
void* __osGetCurrFaultedThread(void) {
    return NULL;
}

void osAfterPreNMI(void) {
}
void __osSetWatchLo(uint32_t value) {
}
void osWritebackDCache(void* vaddr, int32_t nbytes) {
}
void osWritebackDCacheAll(void) {
}
void osInvalDCache(void* vaddr, int32_t nbytes) {
}
void osInvalICache(void* vaddr, int32_t nbytes) {
}

/* Addresses are the same for the CPU and the RCP here */
uint32_t osVirtualToPhysical(void* addr) {
    return (uint32_t)(uintptr_t)addr;
}
void* osPhysicalToVirtual(uint32_t addr) {
    return (void*)(uintptr_t)addr;
}

/* libultra's printf core: the formatting is the C library's (weak: a port that
   builds libultra's libc has its own) */
typedef char* (*outfun)(char*, const char*, size_t);
__attribute__((weak)) int _Printf(outfun prout, char* arg, const char* fmt, va_list args) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);

    if (n < 0) {
        return n;
    }
    if (n >= (int)sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    return prout(arg, buf, n) != NULL ? n : -1;
}

/* Debug output */
void osSyncPrintf(const char* fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void rmonPrintf(const char* fmt, ...) {
}

/* libm under libultra's names (libultra's own take doubles apart as
   big-endian words; weak: a port can build fixed copies) */
__attribute__((weak)) float __sinf(float x) {
    return sinf(x);
}
__attribute__((weak)) float __cosf(float x) {
    return cosf(x);
}
float __floorf(float x) {
    return floorf(x);
}
float __ceilf(float x) {
    return ceilf(x);
}
float __roundf(float x) {
    return roundf(x);
}
float __truncf(float x) {
    return truncf(x);
}
float __nearbyintf(float x) {
    return nearbyintf(x);
}
double __floor(double x) {
    return floor(x);
}
double __ceil(double x) {
    return ceil(x);
}
double __round(double x) {
    return round(x);
}
double __trunc(double x) {
    return trunc(x);
}
double __nearbyint(double x) {
    return nearbyint(x);
}

/* ---------------------------------------------------------------------------
 * Where the threads are (app.c's SIGQUIT): each N64 thread prints its own
 * place on SIGRTMIN + 1
 * ------------------------------------------------------------------------- */

void S30App_PrintWhere(const char* who, void* ctx); // app.c

static void on_where(int sig, siginfo_t* info, void* ctx) {
    S30Thread* t = tSelf;
    char who[48];

    snprintf(who, sizeof(who), "thread %d%s", t != NULL ? (int)t->id : -1, t == sRunning ? " (on the CPU)" : "");
    S30App_PrintWhere(who, ctx);
}

void S30Os_DumpThreads(void) {
    static int installed;
    int i;

    if (!installed) {
        struct sigaction sa;

        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = on_where;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(SIGRTMIN + 1, &sa, NULL);
        installed = 1;
    }
    for (i = 0; i < MAX_THREADS; i++) {
        S30Thread* t = sThreads[i];

        if (t != NULL && t->started) {
            pthread_kill(t->pt, SIGRTMIN + 1);
        }
    }
}
