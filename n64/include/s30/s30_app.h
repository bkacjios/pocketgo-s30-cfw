#ifndef S30_APP_H
#define S30_APP_H

// Process-level pieces every port's main() uses (n64/src/app.c): crash
// reports, fatal errors and the save and data paths.

#include <stdarg.h>

// Crash reports (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT) on stderr and
// appended to crashLog: fault address, registers and the code addresses on
// the stack, for addr2line. SIGQUIT (kill -QUIT <pid>, when a game hangs)
// prints where each of the game's N64 threads is, with s30_os.h.
void S30App_InstallCrashHandler(const char* crashLog);

// A game's fatal error: the message on stderr, then a crash report
void S30App_Fatal(const char* fmt, va_list ap);

// A stack for the signal handlers, so that they run after a stack overflow
// (once per thread; the main thread's comes with the crash handler)
void S30App_SignalStack(void);

// Paths, malloc'd: `name` in `dir`; the save file's default,
// $XDG_DATA_HOME/<game>/<file> or ~/.local/share/<game>/<file> (its
// directories made), else <file> in `fallbackDir`
const char* S30App_PathIn(const char* dir, const char* name);
const char* S30App_SavePath(const char* game, const char* file, const char* fallbackDir);

#endif
