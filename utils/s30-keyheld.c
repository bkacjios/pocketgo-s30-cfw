/*
 * s30-keyheld: exit 0 if a key or button is held down on any input device,
 * or (-w) wait until one is pressed.
 *
 * Usage: s30-keyheld CODE          exit 0 if CODE is held now
 *        s30-keyheld -w CODE...    wait for a press of one of CODE... (one
 *                                  already held counts); prints the code
 * (linux/input-event-codes.h: 314 BTN_SELECT, 315 BTN_START, 305 BTN_EAST
 * = A, 304 BTN_SOUTH = B)
 *
 * -w reads the devices' events, so a short press is never missed; a script
 * that polls the held state ("until s30-keyheld 315; do sleep 0.2; done")
 * only sees a press that lasts over its sleep.
 * Used early (initramfs), so it waits up to 2 s for a device that has the
 * key to appear (the gamepad probes on I2C). Exit 1: not held, 2: no device
 * with that key.
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define TEST_BIT(bits, n) ((bits)[(n) / 8] & (1 << ((n) % 8)))
#define MAX_CODES 8
#define MAX_DEVS 16

/* 0/1: held state from a device with the key, -1: no such device */
static int key_state(int code) {
	DIR *dir = opendir("/dev/input");
	struct dirent *de;
	uint8_t caps[KEY_MAX / 8 + 1], keys[KEY_MAX / 8 + 1];
	char path[300];
	int state = -1, fd;

	if (dir == NULL) {
		return -1;
	}
	while (state != 1 && (de = readdir(dir)) != NULL) {
		if (strncmp(de->d_name, "event", 5) != 0) {
			continue;
		}
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			continue;
		}
		memset(caps, 0, sizeof(caps));
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(caps)), caps) >= 0 && TEST_BIT(caps, code)) {
			// Opening starts a polled device; give it one poll
			usleep(60000);
			memset(keys, 0, sizeof(keys));
			if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) >= 0) {
				state = TEST_BIT(keys, code) ? 1 : 0;
			}
		}
		close(fd);
	}
	closedir(dir);
	return state;
}

static int has_code(int code, int ncodes, const int *codes) {
	int i;

	for (i = 0; i < ncodes; i++) {
		if (codes[i] == code) {
			return 1;
		}
	}
	return 0;
}

/* Wait for a press of one of the codes on any device that has one; one held
   already counts. Prints the code. 0: pressed, -1: no device with a code */
static int key_wait(int ncodes, const int *codes) {
	DIR *dir = opendir("/dev/input");
	struct dirent *de;
	struct pollfd fds[MAX_DEVS];
	struct input_event ev;
	uint8_t caps[KEY_MAX / 8 + 1], keys[KEY_MAX / 8 + 1];
	char path[300];
	int n = 0, i, fd, held = 0;

	if (dir == NULL) {
		return -1;
	}
	while (n < MAX_DEVS && (de = readdir(dir)) != NULL) {
		if (strncmp(de->d_name, "event", 5) != 0) {
			continue;
		}
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
		if (fd < 0) {
			continue;
		}
		memset(caps, 0, sizeof(caps));
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(caps)), caps) < 0) {
			close(fd);
			continue;
		}
		for (i = 0; i < ncodes && !TEST_BIT(caps, codes[i]); i++) {
		}
		if (i == ncodes) {
			close(fd);
			continue;
		}
		fds[n].fd = fd;
		fds[n].events = POLLIN;
		n++;
	}
	closedir(dir);
	if (n == 0) {
		return -1;
	}
	usleep(60000); // a polled device's first poll
	for (i = 0; i < n && !held; i++) {
		memset(keys, 0, sizeof(keys));
		if (ioctl(fds[i].fd, EVIOCGKEY(sizeof(keys)), keys) >= 0) {
			int c;
			for (c = 0; c < ncodes; c++) {
				if (TEST_BIT(keys, codes[c])) {
					held = codes[c];
					break;
				}
			}
		}
	}
	while (!held) {
		if (poll(fds, n, -1) < 0) {
			continue;
		}
		for (i = 0; i < n && !held; i++) {
			if (!(fds[i].revents & POLLIN)) {
				continue;
			}
			while (read(fds[i].fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
				if (ev.type == EV_KEY && ev.value == 1 && has_code(ev.code, ncodes, codes)) {
					held = ev.code;
					break;
				}
			}
		}
	}
	for (i = 0; i < n; i++) {
		close(fds[i].fd);
	}
	printf("%d\n", held);
	return 0;
}

static void usage(void) {
	fprintf(stderr, "usage: s30-keyheld CODE | s30-keyheld -w CODE...\n");
	exit(2);
}

int main(int argc, char **argv) {
	int codes[MAX_CODES], ncodes = 0, wait = 0, state, i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-w") == 0) {
			wait = 1;
		} else if (ncodes < MAX_CODES && atoi(argv[i]) > 0 && atoi(argv[i]) <= KEY_MAX) {
			codes[ncodes++] = atoi(argv[i]);
		} else {
			usage();
		}
	}
	if (ncodes == 0 || (!wait && ncodes != 1)) {
		usage();
	}
	for (i = 0; i < 20; i++) {
		state = wait ? key_wait(ncodes, codes) : key_state(codes[0]);
		if (state >= 0) {
			return (wait || state) ? 0 : 1;
		}
		usleep(100000);
	}
	return 2;
}
