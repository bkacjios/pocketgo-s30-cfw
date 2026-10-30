/*
 * s30-volumed: system volume from the volume keys (gpio-keys KEY_VOLUMEUP/
 * KEY_VOLUMEDOWN). Steps the codec's "Headphone Playback Volume", which
 * drives both the headphones and the speaker amp, so it applies to every app.
 * The level is restored at start and saved to the file given as argument.
 * Holding a key repeats (gpio-keys-polled has no autorepeat).
 *
 * Usage: s30-volumed STATE_FILE
 */
#include <alsa/asoundlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONTROL "Headphone Playback Volume"
#define DEFAULT_LEVEL 40
#define STEP 3
#define REPEAT_DELAY_MS 400
#define REPEAT_MS 120
#define SAVE_DELAY_MS 2000 // after the last change: one SD card write per adjustment

static snd_ctl_t *sCtl;
static snd_ctl_elem_value_t *sVal;
static long sMax;

static int open_input(void) {
	DIR *dir = opendir("/dev/input");
	struct dirent *de;
	int fd = -1;

	while (dir != NULL && (de = readdir(dir)) != NULL) {
		char path[300], name[64] = "";

		if (strncmp(de->d_name, "event", 5) != 0) {
			continue;
		}
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd >= 0 && ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 && strcmp(name, "gpio-keys") == 0) {
			break;
		}
		if (fd >= 0) {
			close(fd);
		}
		fd = -1;
	}
	if (dir != NULL) {
		closedir(dir);
	}
	return fd;
}

static int open_mixer(void) {
	snd_ctl_elem_info_t *info;

	snd_ctl_elem_info_alloca(&info);
	if (snd_ctl_open(&sCtl, "hw:0", 0) < 0) {
		return -1;
	}
	snd_ctl_elem_value_malloc(&sVal);
	snd_ctl_elem_value_set_interface(sVal, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_value_set_name(sVal, CONTROL);
	snd_ctl_elem_info_set_interface(info, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_info_set_name(info, CONTROL);
	if (snd_ctl_elem_info(sCtl, info) < 0) {
		snd_ctl_close(sCtl);
		snd_ctl_elem_value_free(sVal);
		return -1;
	}
	sMax = snd_ctl_elem_info_get_max(info);
	return 0;
}

static long get_level(void) {
	if (snd_ctl_elem_read(sCtl, sVal) < 0) {
		return -1;
	}
	return snd_ctl_elem_value_get_integer(sVal, 0);
}

static void set_level(long level) {
	level = level < 0 ? 0 : level > sMax ? sMax : level;
	snd_ctl_elem_value_set_integer(sVal, 0, level);
	if (snd_ctl_elem_write(sCtl, sVal) < 0) {
		fprintf(stderr, "s30-volumed: cannot set " CONTROL "\n");
	}
}

static long load_level(const char *path) {
	FILE *f = fopen(path, "r");
	long level = DEFAULT_LEVEL;

	if (f != NULL) {
		if (fscanf(f, "%ld", &level) != 1) {
			level = DEFAULT_LEVEL;
		}
		fclose(f);
	}
	return level;
}

static void save_level(const char *path, long level) {
	char tmp[512];
	FILE *f;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "w");
	if (f == NULL) {
		return;
	}
	fprintf(f, "%ld\n", level);
	if (fclose(f) == 0) {
		rename(tmp, path);
	}
}

int main(int argc, char **argv) {
	struct pollfd pfd = { .events = POLLIN };
	int held = 0; // +1 / -1 while a key is down
	int timeout, saveIn = -1;

	if (argc != 2) {
		fprintf(stderr, "usage: s30-volumed STATE_FILE\n");
		return 2;
	}
	// The sound card and the keys may still be probing at boot
	while ((pfd.fd = open_input()) < 0 || open_mixer() < 0) {
		if (pfd.fd >= 0) {
			close(pfd.fd);
		}
		sleep(1);
	}
	set_level(load_level(argv[1]));

	timeout = -1;
	for (;;) {
		int wait = held ? timeout : saveIn;
		int n = poll(&pfd, 1, wait);

		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return 1;
		}
		if (n == 0) {
			if (held) {
				// Key still down: repeat
				set_level(get_level() + held * STEP);
				timeout = REPEAT_MS;
				saveIn = SAVE_DELAY_MS;
			} else if (saveIn >= 0) {
				save_level(argv[1], get_level());
				saveIn = -1;
			}
			continue;
		}

		struct input_event ev[16];
		ssize_t len = read(pfd.fd, ev, sizeof(ev));

		if (len <= 0) {
			return 1;
		}
		for (size_t i = 0; i < len / sizeof(ev[0]); i++) {
			int dir;

			if (ev[i].type != EV_KEY) {
				continue;
			}
			dir = ev[i].code == KEY_VOLUMEUP ? 1 : ev[i].code == KEY_VOLUMEDOWN ? -1 : 0;
			if (dir == 0) {
				continue;
			}
			if (ev[i].value == 1) {
				held = dir;
				set_level(get_level() + dir * STEP);
				timeout = REPEAT_DELAY_MS;
			} else if (ev[i].value == 0 && held == dir) {
				held = 0;
			}
			saveIn = SAVE_DELAY_MS;
		}
	}
}
