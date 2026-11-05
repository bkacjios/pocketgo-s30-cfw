/*
 * Process-level pieces every port's main() uses (s30_app.h): crash reports,
 * fatal errors and paths.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>

#include "s30_app.h"

/* ---------------------------------------------------------------------------
 * Crash reports
 * ------------------------------------------------------------------------- */

static const char* sCrashLog;
static int sCrashFd = -1;

/* To stderr and the crash log (async-signal-safe calls only) */
static void crash_write(const char* s) {
    write(2, s, strlen(s));
    if (sCrashFd >= 0) {
        write(sCrashFd, s, strlen(s));
    }
}

static void err_write(const char* s) {
    write(2, s, strlen(s));
}

/* The code addresses on the stack, up to 2 KB of it and only while its pages
   are readable (a thread's stack ends at a guard page) */
static void write_stack(const uintptr_t* sp, void (*out)(const char*)) {
    extern char __executable_start[], etext[];
    char buf[16];
    int i, n = 0;

    for (i = 0; i < 512 && n < 24; i++) {
        if (i == 0 || ((uintptr_t)&sp[i] & 0xFFF) == 0) {
            // A new page: its protection, from /proc is too slow here; a
            // write() of it to a pipe tells (EFAULT when unreadable)
            int fds[2];
            int ok;

            if (pipe(fds) != 0) {
                break;
            }
            ok = write(fds[1], &sp[i], sizeof(uintptr_t)) == sizeof(uintptr_t);
            close(fds[0]);
            close(fds[1]);
            if (!ok) {
                break;
            }
        }
        if (sp[i] >= (uintptr_t)__executable_start && sp[i] < (uintptr_t)etext) {
            snprintf(buf, sizeof(buf), " %08lx", (unsigned long)sp[i]);
            out(buf);
            n++;
        }
    }
    out("\n");
}

static void on_crash(int sig, siginfo_t* info, void* ctx) {
    const mcontext_t* mc = &((const ucontext_t*)ctx)->uc_mcontext;
    char buf[160];

    sCrashFd = sCrashLog != NULL ? open(sCrashLog, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644) : -1;
    snprintf(buf, sizeof(buf), "crash: signal %d at %p pc %08lx lr %08lx sp %08lx\ncrash: stack", sig, info->si_addr,
             mc->arm_pc, mc->arm_lr, mc->arm_sp);
    crash_write(buf);
    write_stack((const uintptr_t*)mc->arm_sp, crash_write);
    if (sCrashFd >= 0) {
        fsync(sCrashFd); // the device may be powered off next
        close(sCrashFd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

/* One line for SIGQUIT: where a thread is (os.c calls it on each N64 thread) */
void S30App_PrintWhere(const char* who, void* ctx) {
    const mcontext_t* mc = &((const ucontext_t*)ctx)->uc_mcontext;
    char buf[96];

    snprintf(buf, sizeof(buf), "where: %s pc %08lx lr %08lx stack", who, mc->arm_pc, mc->arm_lr);
    err_write(buf);
    write_stack((const uintptr_t*)mc->arm_sp, err_write);
}

/* The N64 threads (os.c), for ports that run them */
void S30Os_DumpThreads(void) __attribute__((weak));

static void on_quit(int sig, siginfo_t* info, void* ctx) {
    if (S30Os_DumpThreads != NULL) {
        S30Os_DumpThreads();
    } else {
        S30App_PrintWhere("main", ctx);
    }
}

void S30App_SignalStack(void) {
    stack_t ss;

    ss.ss_sp = malloc(64 << 10);
    ss.ss_size = 64 << 10;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
}

void S30App_InstallCrashHandler(const char* crashLog) {
    struct sigaction sa;

    sCrashLog = crashLog;
    S30App_SignalStack();
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sa.sa_sigaction = on_quit;
    sigaction(SIGQUIT, &sa, NULL);
}

void S30App_Fatal(const char* fmt, va_list ap) {
    fprintf(stderr, "fatal: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    abort();
}

/* ---------------------------------------------------------------------------
 * Paths
 * ------------------------------------------------------------------------- */

const char* S30App_PathIn(const char* dir, const char* name) {
    char* p = malloc(strlen(dir) + strlen(name) + 2);

    sprintf(p, "%s/%s", dir, name);
    return p;
}

/* mkdir -p for a path's parent directories */
static void make_parents(const char* path) {
    char* p = strdup(path);
    char* s;

    for (s = strchr(p + 1, '/'); s != NULL; s = strchr(s + 1, '/')) {
        *s = '\0';
        if (mkdir(p, 0755) != 0 && errno != EEXIST) {
            perror(p);
        }
        *s = '/';
    }
    free(p);
}

const char* S30App_SavePath(const char* game, const char* file, const char* fallbackDir) {
    const char* xdg = getenv("XDG_DATA_HOME");
    const char* home = getenv("HOME");
    char* path;

    if (xdg != NULL && xdg[0] == '/') {
        path = malloc(strlen(xdg) + strlen(game) + strlen(file) + 3);
        sprintf(path, "%s/%s/%s", xdg, game, file);
    } else if (home != NULL) {
        path = malloc(strlen(home) + strlen(game) + strlen(file) + 16);
        sprintf(path, "%s/.local/share/%s/%s", home, game, file);
    } else {
        return S30App_PathIn(fallbackDir, file);
    }
    make_parents(path);
    return path;
}
