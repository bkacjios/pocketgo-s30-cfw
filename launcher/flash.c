/*
 * s30-flash: write a boot flash image (build.sh flash, out/spi-nor.bin, or
 * the stock backup) to the SPI NOR flash, /dev/mtd0.
 *
 * Usage: s30-flash write IMAGE    write IMAGE, then verify it
 *        s30-flash verify IMAGE   compare the flash with IMAGE
 *        s30-flash read FILE      save the whole flash to FILE (a backup)
 *
 * The image must be exactly the size of the flash and start with a valid
 * eGON boot header (Allwinner boot0/SPL: magic and checksum), which is what
 * the boot ROM loads. Only erase blocks that differ from the flash are
 * erased and written, so an update that only changes the kernel leaves
 * U-Boot alone. If block 0 (the boot header) changes, it is erased first
 * and written last: until the update is complete the boot ROM finds no
 * valid header and moves on (SD card, then FEL), instead of starting a
 * half-written image. Every block is read back after writing, and the
 * whole flash at the end.
 *
 * While writing, the screen shows the progress as text on the console
 * (tty1), which s30-display shows whenever no app holds the display. Started
 * from the launcher, the launcher has already let the display go. Otherwise
 * (ssh) the frontend is stopped (SIGSTOP, so init does not respawn it) and
 * the launcher and anything it started are killed. At the end the result
 * stays up until a button is pressed (or SIGINT/SIGTERM/SIGHUP), then a
 * stopped frontend continues, which starts the launcher again. Messages
 * also go to stdout/stderr.
 *
 * The boot ROM tries the SD card first, so a card with U-Boot at 8 KiB
 * boots whatever the flash holds. The kernel exposes the flash read-only
 * unless the DTS partition drops "read-only".
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <mtd/mtd-user.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define MTD_DEV "/dev/mtd0"
#define EGON_MAGIC "eGON.BT0"
#define EGON_STAMP 0x5f0a6c39 /* checksum field value while summing */
#define FRONTEND "/usr/libexec/s30/frontend"
#define TTY "/dev/tty1"
#define MAX_INPUTS 16

static FILE *sTty;       /* the console while writing, else NULL */
static pid_t sFrontend;  /* stopped frontend, else 0 */

/* Process whose command line ends with `what` (so not a shell or ssh
   session that merely mentions it in a longer command), or 0 */
static pid_t find_process(const char *what) {
	DIR *d = opendir("/proc");
	struct dirent *de;
	pid_t found = 0;

	while (d && !found && (de = readdir(d))) {
		char path[64], cmd[256] = "";
		size_t n, i;
		FILE *f;

		if (de->d_name[0] < '1' || de->d_name[0] > '9')
			continue;
		snprintf(path, sizeof(path), "/proc/%s/cmdline", de->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		n = fread(cmd, 1, sizeof(cmd) - 1, f);
		fclose(f);
		while (n && !cmd[n - 1])
			n--; /* the last argument's NUL */
		for (i = 0; i < n; i++)
			cmd[i] = cmd[i] ? cmd[i] : ' '; /* arguments are NUL separated */
		cmd[n] = '\0';
		if (n >= strlen(what) && strcmp(cmd + n - strlen(what), what) == 0)
			found = atoi(de->d_name);
	}
	if (d)
		closedir(d);
	return found;
}

/* Parent of `pid` from /proc/PID/stat, or 0 */
static pid_t parent_of(pid_t pid) {
	char path[64], buf[256], *p;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	f = fopen(path, "r");
	if (!f)
		return 0;
	p = fgets(buf, sizeof(buf), f) ? strrchr(buf, ')') : NULL;
	fclose(f);
	/* "pid (comm) state ppid ...": comm may hold spaces, so after ')' */
	return p ? atoi(p + 4) : 0;
}

/* Whether `pid` is one of this process's ancestors */
static int is_ancestor(pid_t pid) {
	pid_t p;

	for (p = getppid(); p > 1; p = parent_of(p))
		if (p == pid)
			return 1;
	return 0;
}

/* SIGKILL every descendant of `pid` (children first found, then theirs) */
static void kill_children(pid_t pid) {
	DIR *d = opendir("/proc");
	struct dirent *de;

	while (d && (de = readdir(d))) {
		pid_t child = atoi(de->d_name);

		if (child > 0 && parent_of(child) == pid) {
			kill_children(child);
			kill(child, SIGKILL);
		}
	}
	if (d)
		closedir(d);
}

static volatile sig_atomic_t sStop;

static void on_signal(int sig) {
	(void)sig;
	sStop = 1;
}

/* Until a key or button goes down on any input device, or a signal */
static void wait_button(void) {
	struct pollfd fds[MAX_INPUTS];
	struct sigaction sa = { .sa_handler = on_signal }; /* no SA_RESTART */
	sigset_t all;
	DIR *d = opendir("/dev/input");
	struct dirent *de;
	int n = 0, i;

	while (d && n < MAX_INPUTS && (de = readdir(d))) {
		char path[300];

		if (strncmp(de->d_name, "event", 5) != 0)
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		fds[n].fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		fds[n].events = POLLIN;
		n += fds[n].fd >= 0;
	}
	if (d)
		closedir(d);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigemptyset(&all);
	sigprocmask(SIG_SETMASK, &all, NULL); /* the write blocked them */
	while (!sStop) {
		if (poll(fds, n, -1) < 0)
			continue; /* EINTR: sStop is set */
		for (i = 0; i < n; i++) {
			struct input_event ev;

			while (read(fds[i].fd, &ev, sizeof(ev)) == sizeof(ev))
				if (ev.type == EV_KEY && ev.value == 1)
					sStop = 1;
		}
	}
	for (i = 0; i < n; i++)
		close(fds[i].fd);
}

/* Keep the result up until a button, then let the frontend start the
   launcher again */
static void screen_end(void) {
	fprintf(sTty, "\n  Press any button to exit.\n");
	printf("Press any button on the device to exit\n");
	fflush(stdout);
	wait_button();
	fclose(sTty);
	if (sFrontend)
		kill(sFrontend, SIGCONT);
}

/* Take the screen for the console: started from the launcher, it already is;
   otherwise stop the frontend and close what it runs */
static void screen_start(const char *path, uint32_t blocks) {
	sFrontend = find_process(FRONTEND);
	if (sFrontend && is_ancestor(sFrontend)) {
		sFrontend = 0;
	} else if (sFrontend) {
		kill(sFrontend, SIGSTOP);
		kill_children(sFrontend);
		usleep(500000); /* s30-display notices the lease is gone */
	}
	sTty = fopen(TTY, "w");
	if (!sTty) {
		if (sFrontend)
			kill(sFrontend, SIGCONT);
		sFrontend = 0;
		return;
	}
	setvbuf(sTty, NULL, _IONBF, 0);
	/* Reset (clears the screen), then the title */
	fprintf(sTty, "\033c\n  Boot flash update\n\n  %s\n  %u blocks to write\n\n"
		"  Do not switch off.\n\n", path, blocks);
	atexit(screen_end);
}

static void die(const char *fmt, const char *arg) {
	fprintf(stderr, "s30-flash: ");
	fprintf(stderr, fmt, arg);
	fputc('\n', stderr);
	if (sTty) {
		fprintf(sTty, "\n\n  Error: ");
		fprintf(sTty, fmt, arg);
		fprintf(sTty, "\n\n  A card with U-Boot still boots the device.\n");
	}
	exit(1);
}

static uint32_t le32(const uint8_t *p) {
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Whole-file read into a new buffer; *size is set to the file size. */
static uint8_t *read_file(const char *path, size_t *size) {
	struct stat st;
	uint8_t *buf;
	int fd = open(path, O_RDONLY);

	if (fd < 0 || fstat(fd, &st) < 0)
		die("cannot open %s", path);
	buf = malloc(st.st_size ? st.st_size : 1);
	if (!buf || read(fd, buf, st.st_size) != st.st_size)
		die("cannot read %s", path);
	close(fd);
	*size = st.st_size;
	return buf;
}

static void read_flash(int fd, uint8_t *buf, uint32_t off, uint32_t len) {
	if (pread(fd, buf, len, off) != (ssize_t)len)
		die("flash read failed: %s", strerror(errno));
}

/* The eGON header the boot ROM checks: magic at 4, checksum at 12 (the sum
 * of all 32-bit words over "length" bytes, with the checksum field counted
 * as EGON_STAMP), length at 16. */
static const char *egon_problem(const uint8_t *img, size_t size) {
	uint32_t len, sum = 0, i;

	if (memcmp(img + 4, EGON_MAGIC, 8) != 0)
		return "no eGON.BT0 boot header at offset 0";
	len = le32(img + 16);
	if (len < 32 || len % 4 || len > size)
		return "eGON header has a bad length";
	for (i = 0; i < len; i += 4)
		sum += i == 12 ? EGON_STAMP : le32(img + i);
	if (sum != le32(img + 12))
		return "eGON header checksum does not match";
	return NULL;
}

static int open_flash(int flags, struct mtd_info_user *info) {
	int fd = open(MTD_DEV, flags);

	if (fd < 0) {
		if (errno == EACCES && (flags & O_ACCMODE) != O_RDONLY)
			die("%s is read-only (\"read-only\" on the DTS partition)", MTD_DEV);
		die("cannot open " MTD_DEV ": %s", strerror(errno));
	}
	if (ioctl(fd, MEMGETINFO, info) < 0)
		die("MEMGETINFO on " MTD_DEV ": %s", strerror(errno));
	if (info->type != MTD_NORFLASH || !info->erasesize || info->size % info->erasesize)
		die("%s is not the expected NOR flash", MTD_DEV);
	return fd;
}

static uint8_t *load_image(const char *path, const struct mtd_info_user *info) {
	size_t size;
	uint8_t *img = read_file(path, &size);
	const char *problem;

	if (size != info->size) {
		fprintf(stderr, "s30-flash: %s is %zu bytes, the flash is %u\n",
			path, size, info->size);
		exit(1);
	}
	problem = egon_problem(img, size);
	if (problem)
		die("not a boot image: %s", problem);
	return img;
}

/* Number of differing bytes between the flash and img. */
static uint32_t compare(int fd, const struct mtd_info_user *info, const uint8_t *img) {
	uint8_t *cur = malloc(info->size);
	uint32_t i, diff = 0;

	if (!cur)
		die("%s", "out of memory");
	read_flash(fd, cur, 0, info->size);
	for (i = 0; i < info->size; i++)
		diff += cur[i] != img[i];
	free(cur);
	return diff;
}

static void erase_block(int fd, uint32_t off, uint32_t len) {
	struct erase_info_user ei = { .start = off, .length = len };

	if (ioctl(fd, MEMERASE, &ei) < 0)
		die("erase failed: %s", strerror(errno));
}

static void write_block(int fd, const uint8_t *img, uint32_t off, uint32_t len,
			uint8_t *check) {
	if (pwrite(fd, img + off, len, off) != (ssize_t)len)
		die("write failed: %s", strerror(errno));
	read_flash(fd, check, off, len);
	if (memcmp(check, img + off, len) != 0) {
		char where[16];

		snprintf(where, sizeof(where), "0x%06x", off);
		die("block at %s reads back wrong", where);
	}
}

static int cmd_write(const char *path) {
	struct mtd_info_user info;
	int fd = open_flash(O_RDWR, &info);
	uint8_t *img = load_image(path, &info);
	uint32_t bs = info.erasesize, nblocks = info.size / bs, b, n = 0, done = 0;
	uint8_t *cur = malloc(info.size), *changed = calloc(nblocks, 1);
	sigset_t block;

	if (!cur || !changed)
		die("%s", "out of memory");
	read_flash(fd, cur, 0, info.size);
	for (b = 0; b < nblocks; b++) {
		changed[b] = memcmp(cur + b * bs, img + b * bs, bs) != 0;
		n += changed[b];
	}
	if (!n) {
		printf("Flash already matches %s\n", path);
		return 0;
	}
	printf("Writing %s: %u of %u blocks (%u KiB) differ\n", path, n, nblocks, bs / 1024);

	/* A signal mid-update would leave the flash half written */
	sigemptyset(&block);
	sigaddset(&block, SIGINT);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGHUP);
	sigprocmask(SIG_BLOCK, &block, NULL);

	screen_start(path, n);
	if (changed[0])
		erase_block(fd, 0, bs);
	for (b = 1; b < nblocks; b++) {
		if (!changed[b])
			continue;
		erase_block(fd, b * bs, bs);
		write_block(fd, img, b * bs, bs, cur);
		if (++done % 64 == 0)
			printf("  %u/%u\n", done, n);
		if (sTty)
			fprintf(sTty, "\r  Writing  %u/%u  %u%%", done, n, done * 100 / n);
	}
	if (changed[0])
		write_block(fd, img, 0, bs, cur);

	if (sTty)
		fprintf(sTty, "\r  Writing  %u/%u  100%%\n  Verifying...\n", n, n);
	if (compare(fd, &info, img))
		die("final verify failed: the flash differs from %s", path);
	if (sTty)
		fprintf(sTty, "\n  Done, verified.\n");
	printf("Done, verified\n");
	return 0;
}

static int cmd_verify(const char *path) {
	struct mtd_info_user info;
	int fd = open_flash(O_RDONLY, &info);
	uint8_t *img = load_image(path, &info);
	uint32_t diff = compare(fd, &info, img);

	if (diff) {
		printf("Flash differs from %s in %u bytes\n", path, diff);
		return 1;
	}
	printf("Flash matches %s\n", path);
	return 0;
}

static int cmd_read(const char *path) {
	struct mtd_info_user info;
	int fd = open_flash(O_RDONLY, &info), out;
	uint8_t *buf = malloc(info.size);

	if (!buf)
		die("%s", "out of memory");
	read_flash(fd, buf, 0, info.size);
	out = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (out < 0)
		die("cannot create %s (it must not exist)", path);
	if (write(out, buf, info.size) != (ssize_t)info.size || fsync(out) < 0 || close(out) < 0)
		die("cannot write %s", path);
	printf("Saved the flash (%u bytes) to %s\n", info.size, path);
	return 0;
}

int main(int argc, char **argv) {
	if (argc == 3 && strcmp(argv[1], "write") == 0)
		return cmd_write(argv[2]);
	if (argc == 3 && strcmp(argv[1], "verify") == 0)
		return cmd_verify(argv[2]);
	if (argc == 3 && strcmp(argv[1], "read") == 0)
		return cmd_read(argv[2]);
	fprintf(stderr, "usage: s30-flash write IMAGE | verify IMAGE | read FILE\n");
	return 2;
}
