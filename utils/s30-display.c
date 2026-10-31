/*
 * s30-display: display broker with an on-screen display (OSD).
 *
 * Apps keep driving the display themselves (KMS), through a DRM lease: the
 * libs30lease.so preload turns their open of the display card (the KMS
 * /dev/dri/card*; the GPU is another card) into a request
 * on this daemon's socket, and the daemon answers with a lease fd for the
 * connector, the CRTC and the primary plane. The daemon keeps an overlay
 * plane for itself and shows OSD messages there (volume, headphones, and
 * external power plugged in or out, watched here from power_supply uevents,
 * with the battery level); the
 * display engine blends it over the app's image, so it costs the app
 * nothing.
 *
 * The overlay plane is enabled once per lease and then left on (transparent
 * when idle): a plane commit on the app's CRTC can make the app's own page
 * flip fail with EBUSY. Frames are drawn into a private buffer and copied
 * into the plane's single buffer right after a vblank (one fast memcpy,
 * before scanout reaches the box; the daemon runs SCHED_FIFO to wake in
 * time), so updates don't tear; the bar eases to a new value and the box
 * fades in and out at the panel rate.
 *
 * The daemon is DRM master only while it has handed out a lease (a lease
 * needs a master lessor), so an app that opens the card directly still
 * works when the daemon is not involved.
 *
 * System menu: the power key (grabbed, so apps never see it) opens a menu
 * over whoever holds the lease: a brightness slider (D-pad left/right; the
 * level is saved to the file given as argument and restored at start), then
 * Resume / Sleep / Exit game over an app, Sleep /
 * Reboot / Shut down / Cancel over the launcher, with the battery (level, and a
 * bolt while charging) in a small box in the screen's top right, where the
 * launcher shows it. That process is frozen (SIGSTOP) and
 * the gamepad grabbed while the menu is open; Exit sends SIGTERM, then
 * SIGKILL if it hasn't gone after EXIT_GRACE_MS. The
 * menu uses the same overlay plane (the display engine blends only one
 * alpha plane), resized to the whole screen (transparent around the menu and
 * battery boxes): those commits happen while the app is frozen, so they can't
 * collide with its page flips.
 *
 * Sleep is s2idle ("freeze" to /sys/power/state, which returns on wake): the
 * menu is cleared first, the app stays stopped until the system is back, and
 * the power key press that woke it is ignored (WAKE_IGNORE_MS).
 *
 * Socket messages (SOCK_SEQPACKET, /run/s30-display.sock):
 *   "lease PATH"             PATH being opened; for the display card reply "ok"
 *                            with the lease fd attached, else "fail"
 *   "osd ICON PERCENT"       ICON: speaker, headphones, mute, charging,
 *                            battery; PERCENT 0-100
 *
 * Volume changes while the menu is open show the volume box above it (eased
 * and faded the same way).
 *
 * Usage: s30-display [BRIGHTNESS_FILE]
 */
#define _GNU_SOURCE // struct ucred
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <dirent.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <time.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "osd_font.h"

#define SOCKET_PATH "/run/s30-display.sock"
#define OSD_MS 1500       // OSD visible after the last message
#define FADE_STEP 0.17f   // alpha per frame: about 100 ms fades
#define EASE 0.3f         // fraction of the remaining bar distance per frame
#define LEASE_POLL_MS 250 // check whether the lessee has gone
#define EXIT_GRACE_MS 2000 // after SIGTERM, before SIGKILL
#define WAKE_IGNORE_MS 500 // power key presses after waking: the one that woke us
#define SAVE_DELAY_MS 2000 // brightness saved this long after the last change
#define BRIGHTNESS_STEPS 32   // slider positions from the minimum to full
#define BRIGHTNESS_MIN_PCT 25 // of the backlight's range: lower is all but off
#define REPEAT_DELAY_MS 400   // holding left/right on the slider repeats
#define REPEAT_MS 60

/* The panel is 320x480 portrait, held landscape with its +x pointing down:
   landscape (x right, y down; 480x320) to panel is px = y, py = 479 - x */
#define PANEL_W 320
#define PANEL_H 480

/* The overlay buffer, the whole landscape screen: the OSD box uses its top
   left BOX_W x BOX_H, the menu all of it (the plane shows only the part in
   use, and only that part is copied in) */
#define BUF_W PANEL_H
#define BUF_H PANEL_W

/* OSD box in landscape coordinates: centred near the top */
#define BOX_W 220
#define BOX_H 40
#define BOX_X ((PANEL_H - BOX_W) / 2)
#define BOX_Y 12

/* Menu box: centred */
#define MENU_W 160
#define MENU_ROW_H 28
#define MENU_PAD 6 // above the first row and below the last
#define MENU_X ((PANEL_H - MENU_W) / 2)

static int sFd = -1;
static char sCard[32]; // the display (KMS) card
static uint32_t sConnector, sCrtc, sPrimary, sOverlay;
static uint32_t sLessee;     // 0: no lease
static int sMaster;
static uint32_t sFb;         // overlay buffer, panel orientation (BUF_H x BUF_W)
static uint32_t* sPixels;    // the plane's buffer (scanned out)
static uint32_t sPitch;      // in pixels
static uint32_t sBack[BUF_W * BUF_H]; // frame being drawn, panel layout, pitch BUF_H
static int sShowW = BOX_W, sShowH = BOX_H; // part of the buffer the plane shows (landscape, from 0, 0)
static int sPlaneOn;          // overlay plane enabled (for the current lease)
static char sIcon[16] = "speaker";
static float sTarget, sShown; // bar percent: requested, drawn
static float sAlpha;          // box opacity drawn
static int sDirty;            // needs a redraw
static uint64_t sOsdUntil, sNextLeaseCheck;
static pid_t sAppPid;         // process holding the lease
static int sMenu, sMenuSel;   // system menu open, selected item
static int sMenuLauncher;     // the menu is over the launcher (power menu)
static int sPadFd = -1;       // gamepad, grabbed while the menu is open
static pid_t sKillPid;        // exiting app: SIGKILL at sKillAt if still there
static uint64_t sKillAt;
static uint64_t sWokeAt;      // last return from sleep
static char sBacklight[300];  // backlight sysfs directory, "" if none
static int sBright, sBrightMax; // backlight level wanted (brightness_min()..sBrightMax)
static float sBrightShown;      // level in the backlight and on the slider, easing to sBright
static int sBrightWritten = -1; // last level written to sysfs
static int sRepeatDir;          // slider key held: -1 left, 1 right, 0 none
static uint64_t sRepeatAt;
static const char* sBrightFile; // where the level is saved, or NULL
static uint64_t sBrightSaveAt;

static uint64_t now_ms(void) {
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static uint64_t plane_type(uint32_t plane) {
	drmModeObjectProperties* props = drmModeObjectGetProperties(sFd, plane, DRM_MODE_OBJECT_PLANE);
	uint64_t type = (uint64_t)-1;
	uint32_t i;

	for (i = 0; props != NULL && i < props->count_props; i++) {
		drmModePropertyRes* p = drmModeGetProperty(sFd, props->props[i]);
		if (p != NULL && strcmp(p->name, "type") == 0) {
			type = props->prop_values[i];
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return type;
}

/* Connector, its CRTC, the primary plane and the topmost ARGB8888 overlay */
static int find_objects(void) {
	drmModeRes* res = drmModeGetResources(sFd);
	drmModePlaneRes* planes = drmModeGetPlaneResources(sFd);
	int crtcIndex = -1, i;
	uint32_t j;

	if (res == NULL || planes == NULL) {
		return -1;
	}
	for (i = 0; i < res->count_connectors && sConnector == 0; i++) {
		drmModeConnector* c = drmModeGetConnector(sFd, res->connectors[i]);
		if (c != NULL && c->connection == DRM_MODE_CONNECTED) {
			sConnector = c->connector_id;
		}
		drmModeFreeConnector(c);
	}
	if (res->count_crtcs > 0) {
		crtcIndex = 0;
		sCrtc = res->crtcs[0];
	}
	for (j = 0; j < planes->count_planes && crtcIndex >= 0; j++) {
		drmModePlane* p = drmModeGetPlane(sFd, planes->planes[j]);
		uint64_t type = plane_type(planes->planes[j]);
		uint32_t f;
		int argb = 0;

		if (p == NULL || !(p->possible_crtcs & (1u << crtcIndex))) {
			drmModeFreePlane(p);
			continue;
		}
		for (f = 0; f < p->count_formats; f++) {
			argb |= p->formats[f] == DRM_FORMAT_ARGB8888;
		}
		if (type == DRM_PLANE_TYPE_PRIMARY && sPrimary == 0) {
			sPrimary = p->plane_id;
		} else if (type == DRM_PLANE_TYPE_OVERLAY && argb) {
			sOverlay = p->plane_id; // the last one listed is the topmost
		}
		drmModeFreePlane(p);
	}
	drmModeFreePlaneResources(planes);
	drmModeFreeResources(res);
	return sConnector && sCrtc && sPrimary && sOverlay ? 0 : -1;
}

static int create_osd_buffer(void) {
	struct drm_mode_create_dumb create = { .width = BUF_H, .height = BUF_W, .bpp = 32 };
	struct drm_mode_map_dumb map = { 0 };
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };

	if (drmIoctl(sFd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
		return -1;
	}
	sPitch = create.pitch / 4;
	handles[0] = create.handle;
	pitches[0] = create.pitch;
	if (drmModeAddFB2(sFd, BUF_H, BUF_W, DRM_FORMAT_ARGB8888, handles, pitches, offsets, &sFb, 0) != 0) {
		return -1;
	}
	map.handle = create.handle;
	if (drmIoctl(sFd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
		return -1;
	}
	sPixels = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, sFd, map.offset);
	return sPixels == MAP_FAILED ? -1 : 0;
}

/* ---------------------------------------------------------------------------
 * Drawing, in buffer-local landscape coordinates (BUF_W x BUF_H)
 * ------------------------------------------------------------------------- */

static void put(int x, int y, uint32_t argb) {
	if (x >= 0 && x < BUF_W && y >= 0 && y < BUF_H) {
		// landscape (x, y) -> panel (y, 479 - x), relative to the buffer
		sBack[(BUF_W - 1 - x) * BUF_H + y] = argb;
	}
}

/* Colour 0xRRGGBB at opacity a (0-1) */
static uint32_t argb(uint32_t rgb, float a) {
	return ((uint32_t)(a * 255.0f + 0.5f) << 24) | rgb;
}

static void fill(int x0, int y0, int w, int h, uint32_t argb) {
	int x, y;

	for (y = y0; y < y0 + h; y++) {
		for (x = x0; x < x0 + w; x++) {
			put(x, y, argb);
		}
	}
}

/* 16x16 icons (the OSD's at 2x) */
static const char* const kSpeaker[16] = {
	"................", "........#.......", ".......##.......", "......###...#...",
	".....####....#..", "########.#....#.", "########..#...#.", "########..#...#.",
	"########..#...#.", "########.#....#.", ".....####....#..", "......###...#...",
	".......##.......", "........#.......", "................", "................",
};
static const char* const kMute[16] = {
	"................", "........#.......", ".......##.......", "......###.......",
	".....####.......", "########..#...#.", "########...#.#..", "########....#...",
	"########...#.#..", "########..#...#.", ".....####.......", "......###.......",
	".......##.......", "........#.......", "................", "................",
};
static const char* const kBattery[16] = {
	"................", "................", "................", "................",
	".#############..", ".#...........#..", ".#...........###", ".#...........###",
	".#...........###", ".#...........###", ".#...........#..", ".#############..",
	"................", "................", "................", "................",
};
static const char* const kCharging[16] = {
	"................", "................", "................", "................",
	".#############..", ".#.....##....#..", ".#....##.....###", ".#...#####...###",
	".#.....##....###", ".#....##.....###", ".#...........#..", ".#############..",
	"................", "................", "................", "................",
};
static const char* const kHeadphones[16] = {
	"................", ".....######.....", "...##......##...", "..#..........#..",
	".#............#.", ".#............#.", ".#............#.", "##............##",
	"###..........###", "####........####", "####........####", "####........####",
	"####........####", ".##..........##.", "................", "................",
};

static const char* const kSun[16] = {
	"................", ".......#........", "..#....#....#...", "...#.......#....",
	"......###.......", ".....#####......", "....#######.....", ".##.#######.##..",
	"....#######.....", ".....#####......", "......###.......", "...#.......#....",
	"..#....#....#...", ".......#........", "................", "................",
};

static void draw_icon(const char* const* icon, int x0, int y0, int scale, uint32_t argb) {
	int x, y;

	for (y = 0; y < 16; y++) {
		for (x = 0; x < 16; x++) {
			if (icon[y][x] == '#') {
				fill(x0 + x * scale, y0 + y * scale, scale, scale, argb);
			}
		}
	}
}

/* Dark rounded box over (x0, y0, w, h) */
static void draw_box(int x0, int y0, int w, int h, float alpha) {
	const int corner[4] = { 3, 2, 1, 1 };
	int i;

	fill(x0, y0, w, h, argb(0x101018, alpha));
	for (i = 0; i < 4; i++) {
		fill(x0, y0 + i, corner[i], 1, 0);
		fill(x0 + w - corner[i], y0 + i, corner[i], 1, 0);
		fill(x0, y0 + h - 1 - i, corner[i], 1, 0);
		fill(x0 + w - corner[i], y0 + h - 1 - i, corner[i], 1, 0);
	}
}

/* ASCII text in the 8x16 font, `scale` pixels per font pixel */
static void draw_text(const char* text, int x0, int y0, int scale, uint32_t argb) {
	int gx, gy;

	for (; *text != '\0'; text++, x0 += 8 * scale) {
		const unsigned char* g = kOsdFont[(*text < 0x20 || *text > 0x7e ? '?' : *text) - 0x20];
		for (gy = 0; gy < 16; gy++) {
			for (gx = 0; gx < 8; gx++) {
				if (g[gy] & (0x80 >> gx)) {
					fill(x0 + gx * scale, y0 + gy * scale, scale, scale, argb);
				}
			}
		}
	}
}

/* The box at (ox, oy) at opacity `alpha`, bar at `percent` (fractional: the
   bar's end pixel is blended, so it moves smoothly) */
static void draw_osd(int ox, int oy, const char* icon, float percent, float alpha) {
	const int barX = 52, barW = BOX_W - barX - 14, barY = (BOX_H - 8) / 2;
	const uint32_t track = 0x404048, bar = 0x8060e0;
	int full;
	float end, frac;

	if (alpha <= 0.0f) {
		fill(0, 0, BUF_W, BUF_H, 0);
		return;
	}
	percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	draw_box(ox, oy, BOX_W, BOX_H, 0.75f * alpha);
	draw_icon(strcmp(icon, "headphones") == 0 ? kHeadphones
	          : strcmp(icon, "mute") == 0     ? kMute
	          : strcmp(icon, "charging") == 0 ? kCharging
	          : strcmp(icon, "battery") == 0  ? kBattery
	                                          : kSpeaker,
	          ox + 10, oy + (BOX_H - 32) / 2, 2, argb(0xffffff, alpha));
	end = barW * percent / 100.0f;
	full = (int)end;
	frac = end - full;
	fill(ox + barX, oy + barY, barW, 8, argb(track, alpha));
	fill(ox + barX, oy + barY, full, 8, argb(bar, alpha));
	if (full < barW && frac > 0.0f) {
		// end pixel: bar over track by coverage
		uint32_t r = (uint32_t)(((bar >> 16) & 0xff) * frac + ((track >> 16) & 0xff) * (1 - frac));
		uint32_t g = (uint32_t)(((bar >> 8) & 0xff) * frac + ((track >> 8) & 0xff) * (1 - frac));
		uint32_t b = (uint32_t)((bar & 0xff) * frac + (track & 0xff) * (1 - frac));
		fill(ox + barX + full, oy + barY, 1, 8, argb((r << 16) | (g << 8) | b, alpha));
	}
}

static const char* const kBolt[7] = { "...##", "..##.", ".##..", "#####", "..##.", ".##..", "##..." };

static int external_power(int* capacity, int* charging);

/* Battery in a small box in the screen's top right, where the launcher
   shows it (right edge 10 px in, centred 16 px down): a battery filled to
   the level (green, yellow, red) with the percentage inside, and a bolt to
   its left while charging (the battery itself never moves) */
static void draw_battery(void) {
	const int bw = 42, bh = 18, right = PANEL_H - 10, y = 16 - bh / 2, pad = 5;
	int capacity, charging, len, x, left, i, j;
	char text[8];
	uint32_t level;

	external_power(&capacity, &charging);
	if (capacity < 0) {
		return; // no fuel gauge
	}
	capacity = capacity > 100 ? 100 : capacity;
	level = capacity > 50 ? 0x46c85a : capacity > 20 ? 0xebbe28 : 0xe13c32;
	x = right - 3 - bw;
	left = charging ? x - 4 - 10 : x;
	draw_box(left - pad, y - pad, right - left + 2 * pad, bh + 2 * pad, 0.92f);
	// Outline only: the empty part shows the box, as translucent as the menu
	fill(x, y, bw, 1, argb(0xc8c8c8, 1.0f));
	fill(x, y + bh - 1, bw, 1, argb(0xc8c8c8, 1.0f));
	fill(x, y + 1, 1, bh - 2, argb(0xc8c8c8, 1.0f));
	fill(x + bw - 1, y + 1, 1, bh - 2, argb(0xc8c8c8, 1.0f));
	fill(x + 2, y + 2, (bw - 4) * capacity / 100, bh - 4, argb(level, 1.0f));
	fill(x + bw, y + 5, 3, bh - 10, argb(0xc8c8c8, 1.0f)); // nub
	// Percentage inside, shadowed (the digits are font rows 2-11: centred)
	len = snprintf(text, sizeof(text), "%d%%", capacity);
	draw_text(text, x + (bw - len * 8) / 2 + 1, y + bh / 2 - 7 + 1, 1, argb(0x000000, 1.0f));
	draw_text(text, x + (bw - len * 8) / 2, y + bh / 2 - 7, 1, argb(0xffffff, 1.0f));
	if (charging) {
		for (j = 0; j < 7; j++) {
			for (i = 0; i < 5; i++) {
				if (kBolt[j][i] == '#') {
					fill(left + i * 2, y + 2 + j * 2, 2, 2, argb(0xffffff, 1.0f));
				}
			}
		}
	}
}

/* Row 0 of both menus is the brightness slider (NULL) */
enum { APP_BRIGHTNESS, APP_RESUME, APP_SLEEP, APP_EXIT };
static const char* const kAppItems[] = { NULL, "Resume", "Sleep", "Exit game" };
enum { SYS_BRIGHTNESS, SYS_SLEEP, SYS_REBOOT, SYS_POWEROFF, SYS_CANCEL };
static const char* const kSysItems[] = { NULL, "Sleep", "Reboot", "Shut down", "Cancel" };

static int menu_count(void) {
	return sMenuLauncher ? 5 : 4;
}

static int brightness_min(void);
static int brightness_step(void);

/* Colour a over b by coverage f (0-1) */
static uint32_t mix(uint32_t a, uint32_t b, float f) {
	uint32_t r = (uint32_t)(((a >> 16) & 0xff) * f + ((b >> 16) & 0xff) * (1 - f));
	uint32_t g = (uint32_t)(((a >> 8) & 0xff) * f + ((b >> 8) & 0xff) * (1 - f));

	return (r << 16) | (g << 8) | (uint32_t)((a & 0xff) * f + (b & 0xff) * (1 - f));
}

/* Brightness row at y: sun, then a bar like the OSD's (white on the
   selection, which is the bar's purple) at the easing level, its end pixel
   blended; the minimum shows one step */
static void draw_brightness_row(int y, int selected) {
	const int barX = MENU_X + 36, barW = MENU_W - 36 - 12, barY = y + (MENU_ROW_H - 8) / 2;
	const int lo = brightness_min(), step = brightness_step();
	const uint32_t track = selected ? 0x3a2a70 : 0x404048, bar = selected ? 0xffffff : 0x8060e0;
	float end = sBrightMax > lo ? barW * (sBrightShown - lo + step) / (sBrightMax - lo + step) : barW;
	int full;

	end = end < 0 ? 0 : end > barW ? barW : end;
	full = (int)end;
	draw_icon(kSun, MENU_X + 12, y + (MENU_ROW_H - 16) / 2, 1, argb(0xffffff, 1.0f));
	fill(barX, barY, barW, 8, argb(track, 1.0f));
	fill(barX, barY, full, 8, argb(bar, 1.0f));
	if (full < barW && end > full) {
		fill(barX + full, barY, 1, 8, argb(mix(bar, track, end - full), 1.0f));
	}
}

/* The B button, 14x14 at 1x: a round button, the letter (6x8) cut in dark */
static const char* const kButton[14] = {
	"....######....", "..##########..", ".############.", ".############.", "##############",
	"##############", "##############", "##############", "##############", "##############",
	".############.", ".############.", "..##########..", "....######....",
};
static const char* const kLetterB[8] = { "#####.", "##..##", "##..##", "#####.", "##..##", "##..##", "##..##", "#####." };

static void draw_button_b(int x0, int y0) {
	int x, y;

	for (y = 0; y < 14; y++) {
		for (x = 0; x < 14; x++) {
			if (kButton[y][x] == '#') {
				int lx = x - 4, ly = y - 3; // the letter's cell
				int letter = lx >= 0 && lx < 6 && ly >= 0 && ly < 8 && kLetterB[ly][lx] == '#';
				put(x0 + x, y0 + y, argb(letter ? 0x101018 : 0xc8c8c8, 1.0f));
			}
		}
	}
}

/* Top of menu row i */
static int menu_row_y(int i) {
	const int h = menu_count() * MENU_ROW_H + 2 * MENU_PAD;

	return (PANEL_W - h) / 2 + MENU_PAD + i * MENU_ROW_H;
}

static void draw_menu(void) {
	const char* const* items = sMenuLauncher ? kSysItems : kAppItems;
	const int rowH = MENU_ROW_H, count = menu_count(), h = count * rowH + 2 * MENU_PAD;
	const int y0 = (PANEL_W - h) / 2, top = y0 + MENU_PAD;
	int i;

	draw_box(MENU_X, y0, MENU_W, h, 0.92f);
	draw_battery();
	fill(BOX_X, BOX_Y, BOX_W, BOX_H, 0); // the OSD box above the menu, as osd_frame left it
	if (sAlpha > 0.0f) {
		draw_osd(BOX_X, BOX_Y, sIcon, sShown, sAlpha);
	}
	for (i = 0; i < count; i++) {
		int y = top + i * rowH;

		if (i == sMenuSel) {
			fill(MENU_X + 6, y + 1, MENU_W - 12, rowH - 2, argb(0x8060e0, 1.0f));
		}
		if (items[i] == NULL) {
			draw_brightness_row(y, i == sMenuSel);
		} else {
			int len = (int)strlen(items[i]), x = MENU_X + (MENU_W - len * 8) / 2;

			draw_text(items[i], x, y + (rowH - 16) / 2, 1, argb(0xffffff, 1.0f));
			if (sMenuLauncher ? i == SYS_CANCEL : i == APP_RESUME) {
				draw_button_b(x - 6 - 14, y + (rowH - 14) / 2); // what B does
			}
		}
	}
}

/* ---------------------------------------------------------------------------
 * Master, lease and plane
 * ------------------------------------------------------------------------- */

static void set_master(int on) {
	if (on != sMaster && (on ? drmSetMaster(sFd) : drmDropMaster(sFd)) == 0) {
		sMaster = on;
	}
}

/* Back buffer to the scanned-out one: the part the plane shows (landscape
   x < sShowW is panel rows from BUF_W - sShowW, y < sShowH their first
   sShowH pixels). The rest stays transparent: it is cleared while shown. */
static void present(void) {
	int row;

	for (row = BUF_W - sShowW; row < BUF_W; row++) {
		memcpy(&sPixels[row * sPitch], &sBack[row * BUF_H], sShowH * sizeof(uint32_t));
	}
}

/* Only landscape (lx, ly, lw, lh) of the back buffer: panel rows
   BUF_W - lx - lw to BUF_W - lx, pixels ly to ly + lh */
static void present_rect(int lx, int ly, int lw, int lh) {
	int row;

	for (row = BUF_W - lx - lw; row < BUF_W - lx; row++) {
		memcpy(&sPixels[row * sPitch + ly], &sBack[row * BUF_H + ly], lh * sizeof(uint32_t));
	}
}

/* Show the buffer's landscape (0, 0, lw, lh) as a box at landscape (lx, ly).
   Panel rectangle: x = ly, y = 479 - (lx + lw - 1); source rows from
   BUF_W - lw. */
static int set_plane(int lx, int ly, int lw, int lh) {
	if (drmModeSetPlane(sFd, sOverlay, sCrtc, sFb, 0, ly, PANEL_H - lx - lw, lh, lw, 0, (BUF_W - lw) << 16, lh << 16,
	                    lw << 16) != 0) {
		fprintf(stderr, "s30-display: overlay: %s\n", strerror(errno));
		return -1;
	}
	sShowW = lw;
	sShowH = lh;
	sPlaneOn = 1;
	return 0;
}

/* Draw now, after the next vblank (no tearing) */
static void wait_vblank(void) {
	drmVBlank vbl = { .request = { .type = DRM_VBLANK_RELATIVE, .sequence = 1 } };

	drmWaitVBlank(sFd, &vbl);
}

static void present_synced(void) {
	wait_vblank();
	present();
}

static void plane_off(void) {
	if (sPlaneOn) {
		drmModeSetPlane(sFd, sOverlay, sCrtc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
		sPlaneOn = 0;
	}
	sAlpha = 0.0f;
}

static void osd_show(const char* icon, int percent) {
	if (!sMaster || sLessee == 0) {
		return; // nobody on screen through a lease (the display isn't ours)
	}
	snprintf(sIcon, sizeof(sIcon), "%s", icon);
	sTarget = percent;
	if (sAlpha <= 0.0f) {
		sShown = percent; // appear at the value; slide only while visible
	}
	sOsdUntil = now_ms() + OSD_MS;
	sDirty = 1;
	if (!sPlaneOn) {
		draw_osd(0, 0, sIcon, sShown, 0.0f); // starts transparent, fades in
		present();
		set_plane(BOX_X, BOX_Y, BOX_W, BOX_H);
	}
}

/* One frame of fading/easing, drawn just after a vblank (no tearing).
   Returns whether more frames are needed. */
static int osd_frame(void) {
	float want = now_ms() < sOsdUntil ? 1.0f : 0.0f;

	if (!sPlaneOn || (!sDirty && sAlpha == want && sShown == sTarget)) {
		return 0;
	}
	if (sAlpha != want) {
		sAlpha += want > sAlpha ? FADE_STEP : -FADE_STEP;
		sAlpha = sAlpha < 0.0f ? 0.0f : sAlpha > 1.0f ? 1.0f : sAlpha;
	}
	sShown += (sTarget - sShown) * EASE;
	if (sShown - sTarget < 0.2f && sTarget - sShown < 0.2f) {
		sShown = sTarget;
	}
	if (sMenu) {
		// Above the menu (the plane is the menu's, the whole screen): redraw
		// and copy in only the box's spot
		fill(BOX_X, BOX_Y, BOX_W, BOX_H, 0);
		if (sAlpha > 0.0f) {
			draw_osd(BOX_X, BOX_Y, sIcon, sShown, sAlpha);
		}
		wait_vblank();
		present_rect(BOX_X, BOX_Y, BOX_W, BOX_H);
	} else {
		draw_osd(0, 0, sIcon, sShown, sAlpha);
		present_synced();
	}
	sDirty = 0;
	return 1;
}

/* ---------------------------------------------------------------------------
 * System menu
 * ------------------------------------------------------------------------- */

/* Input device named `name`, or -1 */
static int open_input(const char* name) {
	char path[32], n[64];
	int i, fd;

	for (i = 0; i < 16; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) {
			continue;
		}
		if (ioctl(fd, EVIOCGNAME(sizeof(n)), n) >= 0 && strcmp(n, name) == 0) {
			return fd;
		}
		close(fd);
	}
	return -1;
}

static int is_launcher(pid_t pid) {
	char path[32], comm[32] = "";
	FILE* f;

	snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
	f = fopen(path, "r");
	if (f != NULL) {
		if (fgets(comm, sizeof(comm), f) == NULL) {
			comm[0] = '\0';
		}
		fclose(f);
	}
	return strncmp(comm, "s30-launcher", 12) == 0;
}

static void menu_open(void) {
	if (!sMaster || sLessee == 0 || sAppPid <= 0) {
		return;
	}
	sMenuLauncher = is_launcher(sAppPid);
	sPadFd = open_input("PocketGo S30 Gamepad");
	if (sPadFd >= 0) {
		ioctl(sPadFd, EVIOCGRAB, 1); // the app stops getting input
	}
	kill(sAppPid, SIGSTOP);
	sMenu = 1;
	sMenuSel = sMenuLauncher ? SYS_CANCEL : APP_RESUME;
	sAlpha = 0.0f;
	sOsdUntil = 0;
	// Move the plane only while it is transparent, then draw the menu: the
	// plane still sits where the OSD box was until the move takes effect
	fill(0, 0, BUF_W, BUF_H, 0);
	present_synced();
	set_plane(0, 0, BUF_W, BUF_H);
	draw_menu();
	present_synced();
}

/* Close the menu; `exitApp`: end the app instead of resuming it */
static void menu_close(int exitApp) {
	if (!sMenu) {
		return;
	}
	sMenu = 0;
	sAlpha = 0.0f; // the OSD box goes with the menu
	sOsdUntil = 0;
	sRepeatDir = 0;
	if (sLessee != 0) {
		// Back to the (transparent) OSD box before the app runs again
		fill(0, 0, BUF_W, BUF_H, 0);
		present_synced();
		set_plane(BOX_X, BOX_Y, BOX_W, BOX_H);
	}
	if (sPadFd >= 0) {
		ioctl(sPadFd, EVIOCGRAB, 0);
		close(sPadFd);
		sPadFd = -1;
	}
	if (sAppPid > 0) {
		kill(sAppPid, SIGCONT);
		if (exitApp) {
			kill(sAppPid, SIGTERM);
			sKillPid = sAppPid;
			sKillAt = now_ms() + EXIT_GRACE_MS;
		}
	}
}

/* Reboot or power off through init (which stops everything and syncs) */
static void run_system(const char* cmd) {
	pid_t pid = fork();

	if (pid == 0) {
		execlp(cmd, cmd, (char*)NULL);
		_exit(127);
	}
}

/* s2idle until a wakeup source (the power key) fires. The menu is cleared
   first so the panel doesn't come back showing it; the app is still stopped
   and resumes when the menu closes after waking. */
static void system_sleep(void) {
	int fd;

	fill(0, 0, BUF_W, BUF_H, 0);
	present_synced();
	sync();
	fd = open("/sys/power/state", O_WRONLY | O_CLOEXEC);
	if (fd < 0 || write(fd, "freeze", 6) != 6) {
		fprintf(stderr, "s30-display: sleep: %s\n", strerror(errno));
	}
	if (fd >= 0) {
		close(fd);
	}
	sWokeAt = now_ms();
}

static void menu_confirm(void) {
	if (sMenuSel == 0) {
		return; // the brightness slider: left/right only
	}
	if ((sMenuLauncher && sMenuSel == SYS_SLEEP) || (!sMenuLauncher && sMenuSel == APP_SLEEP)) {
		system_sleep();
		menu_close(0);
		return;
	}
	if (!sMenuLauncher) {
		menu_close(sMenuSel == APP_EXIT);
		return;
	}
	menu_close(0);
	if (sMenuSel != SYS_CANCEL) {
		run_system(sMenuSel == SYS_REBOOT ? "reboot" : "poweroff");
	}
}

/* ---------------------------------------------------------------------------
 * Brightness: the first backlight in sysfs, levels brightness_min()..max
 * ------------------------------------------------------------------------- */

static int read_int(const char* path) {
	FILE* f = fopen(path, "r");
	int v = -1;

	if (f != NULL) {
		if (fscanf(f, "%d", &v) != 1) {
			v = -1;
		}
		fclose(f);
	}
	return v;
}

/* Lowest slider level: BRIGHTNESS_MIN_PCT of the range (the kernel's
   default table is perceptual, so this is a quarter of full lightness, a few
   percent of the backlight's power) */
static int brightness_min(void) {
	return sBrightMax * BRIGHTNESS_MIN_PCT / 100 > 1 ? sBrightMax * BRIGHTNESS_MIN_PCT / 100 : 1;
}

/* One slider step: BRIGHTNESS_STEPS positions from the minimum to full
   (equal steps look equal on the perceptual table) */
static int brightness_step(void) {
	int step = (sBrightMax - brightness_min()) / (BRIGHTNESS_STEPS - 1);
	return step > 1 ? step : 1;
}

static void brightness_write(int level) {
	char path[320];
	FILE* f;

	snprintf(path, sizeof(path), "%s/brightness", sBacklight);
	f = fopen(path, "w");
	if (f != NULL) {
		fprintf(f, "%d\n", level);
		fclose(f);
	}
	sBrightWritten = level;
}

/* The level to ease to (brightness_frame) */
static void brightness_set(int level) {
	if (sBacklight[0] != '\0') {
		sBright = level < brightness_min() ? brightness_min() : level > sBrightMax ? sBrightMax : level;
	}
}

/* One frame of easing the backlight (and the slider, while the menu is
   open: only its row is copied in) to sBright; paced by the vblank, which
   osd_frame may already have waited for. Returns whether more are needed. */
static int brightness_frame(int vblankDone) {
	int level;

	if (sBacklight[0] == '\0' || sBrightShown == sBright) {
		return 0;
	}
	sBrightShown += (sBright - sBrightShown) * EASE;
	if (sBrightShown - sBright < 0.5f && sBright - sBrightShown < 0.5f) {
		sBrightShown = sBright;
	}
	level = (int)(sBrightShown + 0.5f);
	if (level != sBrightWritten) {
		brightness_write(level);
	}
	if (!vblankDone) {
		wait_vblank();
	}
	if (sMenu) {
		draw_menu();
		present_rect(MENU_X, menu_row_y(0), MENU_W, MENU_ROW_H);
	}
	return 1;
}

/* Find the backlight; restore the saved level */
static void brightness_init(void) {
	DIR* d = opendir("/sys/class/backlight");
	struct dirent* e;
	char path[320];

	while (d != NULL && (e = readdir(d)) != NULL && sBacklight[0] == '\0') {
		if (e->d_name[0] != '.') {
			snprintf(sBacklight, sizeof(sBacklight), "/sys/class/backlight/%s", e->d_name);
		}
	}
	if (d != NULL) {
		closedir(d);
	}
	if (sBacklight[0] == '\0') {
		return;
	}
	snprintf(path, sizeof(path), "%s/max_brightness", sBacklight);
	sBrightMax = read_int(path);
	snprintf(path, sizeof(path), "%s/brightness", sBacklight);
	sBright = read_int(path);
	if (sBrightMax <= 0) {
		sBacklight[0] = '\0';
		return;
	}
	if (sBrightFile != NULL && read_int(sBrightFile) > 0) {
		brightness_set(read_int(sBrightFile));
	}
	sBrightShown = sBright; // at start: straight there
	brightness_write(sBright);
}

/* The level to the brightness file (temp file, then rename) */
static void brightness_save(void) {
	char tmp[300];
	FILE* f;

	if (sBrightFile == NULL) {
		return;
	}
	snprintf(tmp, sizeof(tmp), "%s.tmp", sBrightFile);
	f = fopen(tmp, "w");
	if (f != NULL) {
		fprintf(f, "%d\n", sBright);
		fclose(f);
		rename(tmp, sBrightFile);
	}
}

/* One slider step left (-1) or right (1); saved after SAVE_DELAY_MS */
static void brightness_nudge(int dir) {
	brightness_set(sBright + dir * brightness_step());
	sBrightSaveAt = now_ms() + SAVE_DELAY_MS;
}

/* Gamepad events while the menu is open */
static void menu_input(void) {
	struct input_event ev[16];
	ssize_t n = read(sPadFd, ev, sizeof(ev));
	int i, redraw = 0;

	for (i = 0; n > 0 && i < (int)(n / sizeof(ev[0])); i++) {
		if (ev[i].type != EV_KEY) {
			continue;
		}
		if (ev[i].value == 0 && (ev[i].code == BTN_DPAD_LEFT || ev[i].code == BTN_DPAD_RIGHT)) {
			sRepeatDir = 0; // released
			continue;
		}
		if (ev[i].value != 1) {
			continue;
		}
		switch (ev[i].code) {
			case BTN_DPAD_UP:
			case BTN_DPAD_DOWN:
				sMenuSel = (sMenuSel + (ev[i].code == BTN_DPAD_UP ? menu_count() - 1 : 1)) % menu_count();
				sRepeatDir = 0;
				redraw = 1;
				break;
			case BTN_DPAD_LEFT:
			case BTN_DPAD_RIGHT:
				if (sMenuSel == 0 && sBacklight[0] != '\0') {
					sRepeatDir = ev[i].code == BTN_DPAD_RIGHT ? 1 : -1;
					sRepeatAt = now_ms() + REPEAT_DELAY_MS;
					brightness_nudge(sRepeatDir);
					redraw = 1;
				}
				break;
			case BTN_EAST: // A: confirm
				menu_confirm();
				return;
			case BTN_SOUTH: // B: back
				menu_close(0);
				return;
		}
	}
	if (redraw) {
		draw_menu();
		present_synced();
	}
}

/* Power key: open or close the menu */
static void power_key(int fd) {
	struct input_event ev[16];
	ssize_t n = read(fd, ev, sizeof(ev));
	int i;

	for (i = 0; n > 0 && i < (int)(n / sizeof(ev[0])); i++) {
		if (ev[i].type == EV_KEY && ev[i].code == KEY_POWER && ev[i].value == 1 &&
		    now_ms() - sWokeAt >= WAKE_IGNORE_MS) {
			if (sMenu) {
				menu_close(0);
			} else {
				menu_open();
			}
		}
	}
}

/* Forget the lease once its lessee has closed it; drop master when idle */
static void check_lease(void) {
	drmModeLesseeListRes* list;
	uint32_t i;
	int alive = 0;

	if (sLessee == 0) {
		return;
	}
	list = drmModeListLessees(sFd);
	for (i = 0; list != NULL && i < list->count; i++) {
		alive |= list->lessees[i] == sLessee;
	}
	drmFree(list);
	if (!alive) {
		menu_close(0);
		plane_off();
		sLessee = 0;
		sAppPid = 0;
		set_master(0);
	}
}

static int make_lease(void) {
	uint32_t objects[3] = { sConnector, sCrtc, sPrimary };
	int fd;

	check_lease(); // a lessee that just closed its fd is gone now
	if (sLessee != 0) {
		return -1; // one app at a time; a second open falls back to a plain fd
	}
	set_master(1);
	if (!sMaster) {
		return -1; // someone opened the card directly and is master
	}
	fd = drmModeCreateLease(sFd, objects, 3, O_CLOEXEC, &sLessee);
	if (fd < 0) {
		sLessee = 0;
		set_master(0);
	}
	return fd;
}

/* ---------------------------------------------------------------------------
 * External power (power_supply uevents): shown when plugged in or out only
 * ------------------------------------------------------------------------- */

#define POWER_SUPPLY "/sys/class/power_supply"

static int read_sysfs(const char* dir, const char* attr, char* buf, size_t len) {
	char path[256];
	FILE* f;
	int ok;

	snprintf(path, sizeof(path), POWER_SUPPLY "/%s/%s", dir, attr);
	f = fopen(path, "r");
	if (f == NULL) {
		return 0;
	}
	ok = fgets(buf, len, f) != NULL;
	fclose(f);
	buf[strcspn(buf, "\n")] = '\0';
	return ok;
}

/* Whether a mains or USB supply is online; battery capacity in *capacity
   (-1 if none), *charging whether its status is Charging ("Full" is not) */
static int external_power(int* capacity, int* charging) {
	DIR* d = opendir(POWER_SUPPLY);
	struct dirent* e;
	char buf[32];
	int online = 0;

	*capacity = -1;
	*charging = 0;
	while (d != NULL && (e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.' || !read_sysfs(e->d_name, "type", buf, sizeof(buf))) {
			continue;
		}
		if (strcmp(buf, "Battery") == 0) {
			if (read_sysfs(e->d_name, "capacity", buf, sizeof(buf))) {
				*capacity = atoi(buf);
			}
			if (read_sysfs(e->d_name, "status", buf, sizeof(buf))) {
				*charging = strcmp(buf, "Charging") == 0;
			}
		} else if (read_sysfs(e->d_name, "online", buf, sizeof(buf)) && atoi(buf) == 1) {
			online = 1;
		}
	}
	if (d != NULL) {
		closedir(d);
	}
	return online;
}

static int sPowerOnline = -1;

/* A uevent arrived: show the OSD if external power came or went; keep the
   menu's battery line current */
static void power_changed(int nl) {
	char buf[2048];
	ssize_t n = recv(nl, buf, sizeof(buf) - 1, MSG_DONTWAIT);
	int online, capacity, charging, i, power = 0;

	if (n <= 0) {
		return;
	}
	buf[n] = '\0';
	for (i = 0; i < n; i += strlen(buf + i) + 1) { // NUL-separated KEY=value
		power |= strcmp(buf + i, "SUBSYSTEM=power_supply") == 0;
	}
	if (!power) {
		return;
	}
	if (sMenu) {
		draw_menu();
		present_synced();
	}
	online = external_power(&capacity, &charging);
	if (online != sPowerOnline) {
		sPowerOnline = online;
		osd_show(online ? "charging" : "battery", capacity < 0 ? 0 : capacity);
	}
}

static int open_uevents(void) {
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = 1 };
	int nl = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
	int capacity, charging;

	if (nl >= 0 && bind(nl, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
		close(nl);
		nl = -1;
	}
	sPowerOnline = external_power(&capacity, &charging); // the state at start is not news
	return nl;
}

/* ---------------------------------------------------------------------------
 * Socket
 * ------------------------------------------------------------------------- */

static void reply(int c, const char* msg, int fd) {
	char cbuf[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { (void*)msg, strlen(msg) };
	struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };

	if (fd >= 0) {
		struct cmsghdr* cm;

		memset(cbuf, 0, sizeof(cbuf));
		mh.msg_control = cbuf;
		mh.msg_controllen = sizeof(cbuf);
		cm = CMSG_FIRSTHDR(&mh);
		cm->cmsg_level = SOL_SOCKET;
		cm->cmsg_type = SCM_RIGHTS;
		cm->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cm), &fd, sizeof(int));
	}
	sendmsg(c, &mh, MSG_NOSIGNAL);
}

static void handle_client(int c) {
	struct ucred cred;
	socklen_t len = sizeof(cred);
	char msg[64], icon[16];
	ssize_t n = recv(c, msg, sizeof(msg) - 1, 0);
	int percent;

	if (n <= 0) {
		return;
	}
	msg[n] = '\0';
	if (strncmp(msg, "lease ", 6) == 0) {
		int fd = strcmp(msg + 6, sCard) == 0 ? make_lease() : -1;
		if (fd >= 0 && getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
			sAppPid = cred.pid; // the app (the shim runs in it)
		}
		reply(c, fd >= 0 ? "ok" : "fail", fd);
		if (fd >= 0) {
			close(fd); // the client has its own copy now
		}
	} else if (sscanf(msg, "osd %15s %d", icon, &percent) == 2) {
		osd_show(icon, percent);
	}
}

int main(int argc, char** argv) {
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int srv, nl, power, i, timeout;

	setvbuf(stdout, NULL, _IOLBF, 0);
	sBrightFile = argc > 1 ? argv[1] : NULL;
	brightness_init();
	{
		// Wake promptly after a vblank (short bursts; blocks otherwise).
		// musl's sched_setscheduler() is a stub: the system call itself.
		struct sched_param sp = { .sched_priority = 1 };
		syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &sp);
	}
	for (i = 0; i < 4 && sFd < 0; i++) { // the KMS card (the GPU has its own)
		snprintf(sCard, sizeof(sCard), "/dev/dri/card%d", i);
		sFd = open(sCard, O_RDWR | O_CLOEXEC);
		if (sFd >= 0 && drmSetClientCap(sFd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0) {
			close(sFd);
			sFd = -1;
		}
	}
	if (sFd < 0) {
		fprintf(stderr, "s30-display: no KMS card\n");
		return 1;
	}
	sMaster = drmIsMaster(sFd);
	set_master(0); // master only while a lease is out
	if (find_objects() != 0 || create_osd_buffer() != 0) {
		fprintf(stderr, "s30-display: no connector/CRTC/planes or OSD buffer\n");
		return 1;
	}

	srv = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);
	unlink(SOCKET_PATH);
	if (srv < 0 || bind(srv, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(srv, 4) != 0) {
		perror("s30-display: " SOCKET_PATH);
		return 1;
	}
	chmod(SOCKET_PATH, 0666);
	nl = open_uevents();
	power = open_input("axp20x-pek");
	if (power >= 0) {
		ioctl(power, EVIOCGRAB, 1); // the power key is the system menu's alone
	}
	printf("s30-display: %s connector %u crtc %u primary %u overlay %u\n", sCard, sConnector, sCrtc, sPrimary,
	       sOverlay);

	for (;;) {
		struct pollfd p[4] = { { srv, POLLIN, 0 }, { nl, POLLIN, 0 }, { power, POLLIN, 0 }, { sPadFd, POLLIN, 0 } };
		uint64_t t = now_ms();
		int animating = osd_frame(); // waits for a vblank when drawing

		animating |= brightness_frame(animating);

		if (t >= sNextLeaseCheck) {
			check_lease();
			sNextLeaseCheck = t + LEASE_POLL_MS;
		}
		if (sBrightSaveAt != 0 && t >= sBrightSaveAt) {
			brightness_save(); // one SD card write per adjustment
			sBrightSaveAt = 0;
		}
		if (sMenu && sRepeatDir != 0 && t >= sRepeatAt) {
			brightness_nudge(sRepeatDir); // slider key held
			sRepeatAt = t + REPEAT_MS;
			draw_menu();
			present_synced();
		}
		if (sKillAt != 0 && t >= sKillAt) {
			if (sKillPid == sAppPid && kill(sKillPid, 0) == 0) {
				kill(sKillPid, SIGKILL); // didn't exit on SIGTERM
			}
			sKillAt = 0;
		}
		// poll() ignores entries with a negative fd
		timeout = animating ? 0 : (int)(sOsdUntil > t && sPlaneOn ? sOsdUntil - t : LEASE_POLL_MS);
		if (sMenu && sRepeatDir != 0) {
			int r = sRepeatAt > t ? (int)(sRepeatAt - t) : 0; // next slider repeat
			timeout = r < timeout ? r : timeout;
		}
		if (poll(p, 4, timeout) <= 0) {
			continue;
		}
		if (power >= 0 && (p[2].revents & POLLIN)) {
			power_key(power);
		}
		if (sMenu && sPadFd >= 0 && (p[3].revents & POLLIN)) {
			menu_input();
		}
		if (nl >= 0 && (p[1].revents & POLLIN)) {
			power_changed(nl);
		}
		if (p[0].revents & POLLIN) {
			int c = accept4(srv, NULL, NULL, SOCK_CLOEXEC);
			if (c >= 0) {
				struct pollfd pc = { c, POLLIN, 0 };
				if (poll(&pc, 1, 1000) > 0) {
					handle_client(c);
				}
				close(c);
			}
		}
	}
}
