/*
 * s30-flash: write a boot flash image (build.sh flash, out/spi-nor.bin, or
 * the stock backup) to the SPI NOR flash, /dev/mtd0.
 *
 * Usage: s30-flash write IMAGE    write IMAGE, then verify it (with a window)
 *        s30-flash restore        write the stock firmware back (STOCK_BACKUP)
 *        s30-flash verify IMAGE   compare the flash with IMAGE
 *        s30-flash read FILE      save the whole flash to FILE (a backup)
 *
 * Stock backup: before write changes a flash that does not hold one of our
 * images (no FIT at FIT_OFFSET) but does hold a boot image (a valid eGON
 * header, so not erased or garbage), the flash is saved to STOCK_BACKUP on
 * the card first, unless that file exists already. So flashing never loses
 * the stock firmware, and restore puts it back.
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
 * write shows a window (the launcher's ui.c): the image, a progress bar,
 * then the result, until a button is pressed (or SIGINT/SIGTERM/SIGHUP).
 * s30-display treats it like the launcher (no performance overlay; the
 * power key menu has no Exit).
 * The display comes through s30-display like any app's: without
 * libs30lease.so in LD_PRELOAD (not started by the launcher) the program
 * runs itself again with it. Started from the launcher, the launcher has
 * let the display go; otherwise (ssh) the frontend is stopped (SIGSTOP, so
 * init does not respawn it) and the launcher and anything it started are
 * killed, and the frontend continues at the end, which starts the launcher
 * again. Messages also go to stdout/stderr.
 *
 * The boot ROM tries the SD card first, so a card with U-Boot at 8 KiB
 * boots whatever the flash holds.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <mtd/mtd-user.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ui.h"

#define MTD_DEV "/dev/mtd0"
#define EGON_MAGIC "eGON.BT0"
#define EGON_STAMP 0x5f0a6c39 /* checksum field value while summing */
#define FRONTEND "/usr/libexec/s30/frontend"
#define LEASE_LIB "/usr/lib/s30/libs30lease.so"
#define STOCK_BACKUP "/mnt/sd/boot/spi-nor-stock.bin"
#define FIT_OFFSET 0xc0000 /* build.sh flash: the FIT (FDT magic d00dfeed) */

static const SDL_Color UI_RED = { 230, 70, 60, 255 };

/* The window's state (sUi: the window is up) */
static bool sUi;
static pid_t sFrontend;          /* stopped frontend, else 0 */
static const char *sTitle = "Boot flash update";
static const char *sImage = "";  /* image being written */
static char sStatus[128] = "Reading the flash...";
static char sNote[160] = "";
static SDL_Color sNoteColor;
static uint32_t sDone, sTotal;   /* blocks written, to write (bar) */
static bool sEnd;                /* finished: "Press any button" */

static void draw(void) {
	const SDL_Rect bar = { 24, 150, UI_W - 48, 18 };
	int w;

	if (!sUi)
		return;
	ui_begin();
	ui_text(sTitle, UI_ACCENT, 24, 24, UI_W - 48);
	ui_text(sImage, UI_DIM, 24, 48, UI_W - 48);
	ui_text(sStatus, UI_TEXT, 24, 120, UI_W - 48);
	ui_box(bar, UI_DIM, 1);
	w = sTotal ? (int)((uint64_t)(bar.w - 4) * sDone / sTotal) : 0;
	ui_fill((SDL_Rect){ bar.x + 2, bar.y + 2, w, bar.h - 4 }, UI_ACCENT);
	ui_text(sNote, sNoteColor, 24, 190, UI_W - 48);
	if (sEnd)
		ui_text("Press any button to exit", UI_DIM, 24, UI_H - 40, UI_W - 48);
	ui_present();
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

/* Whether `pid` is one of this process's ancestors */
static bool is_ancestor(pid_t pid) {
	pid_t p;

	for (p = getppid(); p > 1; p = parent_of(p))
		if (p == pid)
			return true;
	return false;
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

/* Until a gamepad button goes down, or a signal */
static void wait_button(void) {
	static const int codes[] = { BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR,
				     BTN_TL2, BTN_TR2, BTN_SELECT, BTN_START };
	enum { N = sizeof(codes) / sizeof(codes[0]) };
	struct sigaction sa = { .sa_handler = on_signal };
	bool held[N] = { false }, pressed[N];
	sigset_t none;
	int fd = ui_pad_open(), i;

	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL); /* the write blocked them */
	if (fd >= 0)
		ui_pad_poll(fd, held, NULL, codes, N); /* what is already held */
	while (!sStop && fd >= 0) {
		memset(pressed, 0, sizeof(pressed));
		if (ui_pad_poll(fd, held, pressed, codes, N))
			for (i = 0; i < N; i++)
				sStop |= pressed[i];
		SDL_Delay(20);
	}
	if (fd < 0)
		pause(); /* no gamepad: a signal only */
	if (fd >= 0)
		close(fd);
}

/* Show the end state until a button, then let the frontend start the
   launcher again (the window closes with the process) */
static void finish(void) {
	if (!sUi)
		return;
	sEnd = true;
	draw();
	printf("Press any button on the device to exit\n");
	fflush(stdout);
	wait_button();
	ui_video_close();
	sUi = false;
	if (sFrontend)
		kill(sFrontend, SIGCONT);
}

/* The window: started from the launcher, the display is free; otherwise
   stop the frontend and close what it runs. Through s30-display's lease
   (LD_PRELOAD) either way. */
static void window_open(char **argv) {
	const char *pre = getenv("LD_PRELOAD");

	if ((!pre || !strstr(pre, LEASE_LIB)) && access(LEASE_LIB, R_OK) == 0) {
		char self[256];
		ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);

		if (n > 0) {
			self[n] = '\0'; /* the real path: the process keeps its name */
			setenv("LD_PRELOAD", LEASE_LIB, 1);
			execv(self, argv);
		}
		/* else go on without it */
	}
	sFrontend = find_process(FRONTEND);
	if (sFrontend && is_ancestor(sFrontend)) {
		sFrontend = 0;
	} else if (sFrontend) {
		kill(sFrontend, SIGSTOP);
		kill_children(sFrontend);
		usleep(500000); /* s30-display notices the launcher's lease is gone */
	}
	sUi = ui_init();
	if (!sUi) {
		fprintf(stderr, "s30-flash: no display, text only\n");
		if (sFrontend)
			kill(sFrontend, SIGCONT);
		sFrontend = 0;
		return;
	}
	atexit(finish);
	draw();
}

static void die(const char *fmt, const char *arg) {
	fprintf(stderr, "s30-flash: ");
	fprintf(stderr, fmt, arg);
	fputc('\n', stderr);
	snprintf(sStatus, sizeof(sStatus), "Error");
	snprintf(sNote, sizeof(sNote), fmt, arg);
	sNoteColor = UI_RED;
	exit(1); /* finish() shows it */
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

/* In 64 KiB pieces: the SPI controller has no DMA here (the CPU feeds its
   64-byte FIFO), and the kernel times a transfer by its size with a fixed
   ~200 ms margin, so one big read (spi-nor makes 4 MiB transfers) runs out
   of time when the CPU is busy, e.g. with the window starting up */
#define READ_CHUNK 0x10000

static void read_flash(int fd, uint8_t *buf, uint32_t off, uint32_t len) {
	uint32_t done, n;

	for (done = 0; done < len; done += n) {
		n = len - done < READ_CHUNK ? len - done : READ_CHUNK;
		if (pread(fd, buf + done, n, off + done) != (ssize_t)n)
			die("flash read failed: %s", strerror(errno));
	}
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

	if (size != info->size)
		die("%s is not the size of the flash (8 MiB)", path);
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

/* Save the flash's current contents (`cur`) as the stock backup when they
   are a boot image that is not ours and there is no backup yet. Written to
   a temporary name and renamed: never a partial backup. */
static void backup_stock(const uint8_t *cur, uint32_t size) {
	char tmp[sizeof(STOCK_BACKUP) + 4];
	int fd;

	if (le32(cur + FIT_OFFSET) == 0xedfe0dd0 /* d0 0d fe ed */ || egon_problem(cur, size) ||
	    access(STOCK_BACKUP, F_OK) == 0)
		return;
	snprintf(sStatus, sizeof(sStatus), "Saving the stock firmware...");
	draw();
	snprintf(tmp, sizeof(tmp), "%s.new", STOCK_BACKUP);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || write(fd, cur, size) != (ssize_t)size || fsync(fd) < 0 || close(fd) < 0 ||
	    rename(tmp, STOCK_BACKUP) < 0)
		die("cannot save the stock firmware to %s", STOCK_BACKUP);
	printf("Saved the stock firmware to %s\n", STOCK_BACKUP);
}

static int cmd_write(const char *path, char **argv) {
	struct mtd_info_user info;
	int fd;
	uint8_t *img, *cur, *changed;
	uint32_t bs, nblocks, b, n = 0;
	sigset_t block;

	sImage = path;
	window_open(argv); /* first, so every error below shows in it */
	fd = open_flash(O_RDWR, &info);
	img = load_image(path, &info);
	bs = info.erasesize;
	nblocks = info.size / bs;
	cur = malloc(info.size);
	changed = calloc(nblocks, 1);
	if (!cur || !changed)
		die("%s", "out of memory");
	read_flash(fd, cur, 0, info.size);
	for (b = 0; b < nblocks; b++) {
		changed[b] = memcmp(cur + b * bs, img + b * bs, bs) != 0;
		n += changed[b];
	}
	if (!n) {
		printf("Flash already matches %s\n", path);
		snprintf(sStatus, sizeof(sStatus), "Already up to date");
		snprintf(sNote, sizeof(sNote), "The flash holds this image.");
		sNoteColor = UI_TEXT;
		sDone = sTotal = 1;
		return 0;
	}
	backup_stock(cur, info.size);
	printf("Writing %s: %u of %u blocks (%u KiB) differ\n", path, n, nblocks, bs / 1024);

	/* A signal mid-update would leave the flash half written */
	sigemptyset(&block);
	sigaddset(&block, SIGINT);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGHUP);
	sigprocmask(SIG_BLOCK, &block, NULL);

	sTotal = n;
	snprintf(sNote, sizeof(sNote), "Do not switch off.");
	sNoteColor = UI_TEXT;
	if (changed[0])
		erase_block(fd, 0, bs);
	for (b = 1; b < nblocks; b++) {
		if (!changed[b])
			continue;
		snprintf(sStatus, sizeof(sStatus), "Writing  %u of %u blocks", sDone + 1, n);
		draw();
		erase_block(fd, b * bs, bs);
		write_block(fd, img, b * bs, bs, cur);
		if (++sDone % 16 == 0)
			printf("  %u/%u\n", sDone, n);
	}
	if (changed[0]) {
		snprintf(sStatus, sizeof(sStatus), "Writing the boot header");
		draw();
		write_block(fd, img, 0, bs, cur);
		sDone++;
	}

	snprintf(sStatus, sizeof(sStatus), "Verifying...");
	draw();
	if (compare(fd, &info, img))
		die("final verify failed: the flash differs from %s", path);
	snprintf(sStatus, sizeof(sStatus), "Done, verified");
	snprintf(sNote, sizeof(sNote), "The new boot image is in place.");
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
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc == 3 && strcmp(argv[1], "write") == 0)
		return cmd_write(argv[2], argv);
	if (argc == 2 && strcmp(argv[1], "restore") == 0) {
		sTitle = "Restore stock firmware";
		return cmd_write(STOCK_BACKUP, argv);
	}
	if (argc == 3 && strcmp(argv[1], "verify") == 0)
		return cmd_verify(argv[2]);
	if (argc == 3 && strcmp(argv[1], "read") == 0)
		return cmd_read(argv[2]);
	fprintf(stderr, "usage: s30-flash write IMAGE | restore | verify IMAGE | read FILE\n");
	return 2;
}
