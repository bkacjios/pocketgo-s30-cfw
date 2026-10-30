/*
 * s30-keyheld: exit 0 if a key or button is held down on any input device.
 *
 * Usage: s30-keyheld CODE   (linux/input-event-codes.h, e.g. 314 BTN_SELECT)
 *
 * Used early (initramfs), so it waits up to 2 s for a device that has the
 * key to appear (the gamepad probes on I2C). Exit 1: not held, 2: no device
 * with that key.
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define TEST_BIT(bits, n) ((bits)[(n) / 8] & (1 << ((n) % 8)))

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

int main(int argc, char **argv) {
	int code, state, i;

	if (argc != 2 || (code = atoi(argv[1])) <= 0 || code > KEY_MAX) {
		fprintf(stderr, "usage: s30-keyheld CODE\n");
		return 2;
	}
	for (i = 0; i < 20; i++) {
		state = key_state(code);
		if (state >= 0) {
			return state ? 0 : 1;
		}
		usleep(100000);
	}
	return 2;
}
