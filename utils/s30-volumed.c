/*
 * s30-volumed: system volume from the volume keys (gpio-keys KEY_VOLUMEUP/
 * KEY_VOLUMEDOWN). Steps the codec's "Headphone Playback Volume", which
 * drives both the headphones and the speaker amp, so it applies to every app.
 * The level is restored at start and saved to the file given as argument.
 * Holding a key repeats (gpio-keys-polled has no autorepeat).
 * Volume changes and headphones being plugged in or out are shown by
 * s30-display's OSD (output icon and volume bar).
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
#include <sys/socket.h>
#include <sys/un.h>
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

#define OSD_SOCKET "/run/s30-display.sock"

/* Input device whose name is `name`, or ends with it when `suffix` */
static int open_input(const char *want, int suffix) {
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
		if (fd >= 0 && ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0) {
			size_t n = strlen(name), w = strlen(want);
			if (suffix ? n >= w && strcmp(name + n - w, want) == 0 : strcmp(name, want) == 0) {
				break;
			}
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

/* Headphones plugged in, from the codec's jack input device */
static int headphones(int jackFd) {
	unsigned long sw[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = { 0 };

	if (jackFd < 0 || ioctl(jackFd, EVIOCGSW(sizeof(sw)), sw) < 0) {
		return 0;
	}
	return (sw[SW_HEADPHONE_INSERT / (8 * sizeof(long))] >> (SW_HEADPHONE_INSERT % (8 * sizeof(long)))) & 1;
}

/* Show the output and the volume on s30-display's OSD (if it runs) */
static void show_osd(int jackFd) {
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	long level = get_level();
	char msg[48];
	int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);

	if (s < 0) {
		return;
	}
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", OSD_SOCKET);
	snprintf(msg, sizeof(msg), "osd %s %ld",
	         headphones(jackFd) ? "headphones" : level <= 0 ? "mute" : "speaker",
	         sMax > 0 ? level * 100 / sMax : 0);
	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
		send(s, msg, strlen(msg), MSG_NOSIGNAL);
	}
	close(s);
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
	struct pollfd fds[2] = { { .events = POLLIN }, { .fd = -1, .events = POLLIN } };
	struct pollfd *pfd = &fds[0], *jack = &fds[1];
	int held = 0; // +1 / -1 while a key is down
	int timeout, saveIn = -1;

	if (argc != 2) {
		fprintf(stderr, "usage: s30-volumed STATE_FILE\n");
		return 2;
	}
	// The sound card and the keys may still be probing at boot
	while ((pfd->fd = open_input("gpio-keys", 0)) < 0 || open_mixer() < 0) {
		if (pfd->fd >= 0) {
			close(pfd->fd);
		}
		sleep(1);
	}
	set_level(load_level(argv[1]));
	jack->fd = open_input("-jack", 1); // optional: no OSD on plug-in without it

	timeout = -1;
	for (;;) {
		int wait = held ? timeout : saveIn;
		int n = poll(fds, 2, wait);

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
				show_osd(jack->fd);
				timeout = REPEAT_MS;
				saveIn = SAVE_DELAY_MS;
			} else if (saveIn >= 0) {
				save_level(argv[1], get_level());
				saveIn = -1;
			}
			continue;
		}

		struct input_event ev[16];
		ssize_t len;

		if (jack->revents & POLLIN) {
			len = read(jack->fd, ev, sizeof(ev));
			for (size_t i = 0; len > 0 && i < len / sizeof(ev[0]); i++) {
				if (ev[i].type == EV_SW && ev[i].code == SW_HEADPHONE_INSERT) {
					show_osd(jack->fd);
				}
			}
		}
		if (!(pfd->revents & POLLIN)) {
			continue;
		}
		len = read(pfd->fd, ev, sizeof(ev));

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
				show_osd(jack->fd);
				timeout = REPEAT_DELAY_MS;
			} else if (ev[i].value == 0 && held == dir) {
				held = 0;
			}
			saveIn = SAVE_DELAY_MS;
		}
	}
}
