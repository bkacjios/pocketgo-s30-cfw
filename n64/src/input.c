/*
 * N64 controller from the S30 gamepad ("PocketGo S30 Gamepad" evdev device:
 * GPIO buttons + MCU analog stick, 0-255 per axis, Y up = low).
 *
 * The game gives the layout (S30Input_Init: evdev key code -> N64 button
 * bits). L1 + R1 + Select + Start (physical buttons) quits.
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "s30_input.h"

#define GAMEPAD_NAME "PocketGo S30 Gamepad"
#define STICK_MAX 80     // N64 stick range used by the game (about +-80)
#define STICK_DEADZONE 8 // raw units around the centre
#define STICK_MIN_REACH 60 // raw reach assumed until a larger deflection is seen

#define MAX_MAP 32
static S30ButtonMap sButtonMap[MAX_MAP];
static int sMapCount;

static int sFd = -1;
static uint32_t sKeys; // pressed state per sButtonMap entry
static int sQuitKeys;  // L1, R1, Select, Start held (bits 0-3)
static int sAxis[2];  // raw ABS_X, ABS_Y
static int sCentre[2];
static int sReachNeg[2], sReachPos[2];

static int open_gamepad(void) {
    DIR* dir = opendir("/dev/input");
    struct dirent* e;

    if (dir == NULL) {
        return -1;
    }
    while ((e = readdir(dir)) != NULL) {
        char path[300];
        char name[128] = "";
        int fd;

        if (strncmp(e->d_name, "event", 5) != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        if (strcmp(name, GAMEPAD_NAME) == 0) {
            closedir(dir);
            printf("input: %s (%s)\n", name, path);
            return fd;
        }
        close(fd);
    }
    closedir(dir);
    return -1;
}

void S30Input_Init(const S30ButtonMap* map, int count) {
    int i;

    sMapCount = count < MAX_MAP ? count : MAX_MAP;
    memcpy(sButtonMap, map, sMapCount * sizeof(*map));
    sFd = open_gamepad();
    if (sFd < 0) {
        printf("input: no \"%s\" device, controls disabled\n", GAMEPAD_NAME);
        return;
    }
    for (i = 0; i < 2; i++) {
        struct input_absinfo abs;

        ioctl(sFd, EVIOCGABS(i == 0 ? ABS_X : ABS_Y), &abs);
        sAxis[i] = sCentre[i] = abs.value; // the stick rests at startup
        sReachNeg[i] = sReachPos[i] = STICK_MIN_REACH;
    }
}

static int8_t stick_axis(int i) {
    int d = sAxis[i] - sCentre[i];
    int reach;

    // Widen the calibrated range as the stick reaches further
    if (d > sReachPos[i]) {
        sReachPos[i] = d;
    }
    if (-d > sReachNeg[i]) {
        sReachNeg[i] = -d;
    }
    if (d > -STICK_DEADZONE && d < STICK_DEADZONE) {
        return 0;
    }
    reach = d > 0 ? sReachPos[i] : sReachNeg[i];
    return (int8_t)(d * STICK_MAX / reach);
}

void S30Input_Read(S30Pad* pad) {
    struct input_event ev[32];
    ssize_t n;
    size_t i, k;

    while (sFd >= 0 && (n = read(sFd, ev, sizeof(ev))) > 0) {
        for (i = 0; i < n / sizeof(ev[0]); i++) {
            if (ev[i].type == EV_KEY) {
                int q = ev[i].code == BTN_TL ? 1 : ev[i].code == BTN_TR ? 2 : ev[i].code == BTN_SELECT ? 4
                        : ev[i].code == BTN_START ? 8 : 0;

                sQuitKeys = ev[i].value ? (sQuitKeys | q) : (sQuitKeys & ~q);
                for (k = 0; k < (size_t)sMapCount; k++) {
                    if (sButtonMap[k].code == ev[i].code) {
                        sKeys = ev[i].value ? (sKeys | (1u << k)) : (sKeys & ~(1u << k));
                    }
                }
            } else if (ev[i].type == EV_ABS && (ev[i].code == ABS_X || ev[i].code == ABS_Y)) {
                sAxis[ev[i].code == ABS_X ? 0 : 1] = ev[i].value;
            }
        }
    }

    pad->button = 0;
    for (k = 0; k < (size_t)sMapCount; k++) {
        if (sKeys & (1u << k)) {
            pad->button |= sButtonMap[k].button;
        }
    }
    // Exit combo on the physical buttons, whatever they map to
    if (sQuitKeys == 15) {
        printf("input: L1+R1+Select+Start, exiting\n");
        exit(0);
    }
    pad->stick_x = stick_axis(0);
    pad->stick_y = -stick_axis(1); // N64: up is positive
}
