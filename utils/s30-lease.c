/*
 * libs30lease.so: LD_PRELOAD shim that turns opening the display device
 * (a /dev/dri/card*; the daemon knows which one is KMS and refuses the
 * others, such as the GPU) into a DRM lease from s30-display, so the daemon can
 * draw its OSD over any app. Apps need no changes: the lease fd behaves as
 * a master fd for the connector, CRTC and primary plane.
 *
 * Falls back to the real open when the daemon is not running or refuses
 * (then the app is master on its own, as without the shim).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SOCKET_PATH "/run/s30-display.sock"
#define CONNECT_TRIES 20 // 50 ms apart: the daemon may still be starting at boot

static int is_card(const char* path) {
	return path != NULL && strncmp(path, "/dev/dri/card", 13) == 0;
}

/* Lease fd from the daemon for `path`, or -1 */
static int request_lease(const char* path, int flags) {
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	char buf[8], req[64], cbuf[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { buf, sizeof(buf) };
	struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof(cbuf) };
	struct cmsghdr* cm;
	int s, fd = -1, i, saved = errno;
	ssize_t n;

	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);
	s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	for (i = 0; s >= 0 && i < CONNECT_TRIES; i++) {
		if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
			break;
		}
		if (errno != ENOENT && errno != ECONNREFUSED) {
			i = CONNECT_TRIES;
			break;
		}
		nanosleep(&(struct timespec){ 0, 50 * 1000000 }, NULL);
	}
	snprintf(req, sizeof(req), "lease %s", path);
	if (s >= 0 && i < CONNECT_TRIES && send(s, req, strlen(req), MSG_NOSIGNAL) == (ssize_t)strlen(req) &&
	    (n = recvmsg(s, &mh, MSG_CMSG_CLOEXEC)) == 2 && memcmp(buf, "ok", 2) == 0) {
		cm = CMSG_FIRSTHDR(&mh);
		if (cm != NULL && cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
			memcpy(&fd, CMSG_DATA(cm), sizeof(int));
			if (!(flags & O_CLOEXEC)) {
				fcntl(fd, F_SETFD, 0);
			}
		}
	}
	if (s >= 0) {
		close(s);
	}
	errno = saved;
	return fd;
}

int open(const char* path, int flags, ...) {
	static int (*real_open)(const char*, int, ...);
	mode_t mode = 0;
	int fd;

	if (flags & (O_CREAT | O_TMPFILE)) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	if (is_card(path) && (fd = request_lease(path, flags)) >= 0) {
		return fd;
	}
	if (real_open == NULL) {
		real_open = (int (*)(const char*, int, ...))dlsym(RTLD_NEXT, "open");
	}
	return real_open(path, flags, mode);
}

int openat(int dirfd, const char* path, int flags, ...) {
	static int (*real_openat)(int, const char*, int, ...);
	mode_t mode = 0;
	int fd;

	if (flags & (O_CREAT | O_TMPFILE)) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	if (is_card(path) && (fd = request_lease(path, flags)) >= 0) {
		return fd;
	}
	if (real_openat == NULL) {
		real_openat = (int (*)(int, const char*, int, ...))dlsym(RTLD_NEXT, "openat");
	}
	return real_openat(dirfd, path, flags, mode);
}
