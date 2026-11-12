/*
 * s30-display: display broker with an on-screen display (OSD).
 *
 * Apps keep driving the display themselves (KMS), through a DRM lease: the
 * libs30lease.so preload turns their open of the display card (the KMS
 * /dev/dri/card*; the GPU is another card) into a request
 * on this daemon's socket, and the daemon answers with a lease fd for the
 * connector, the CRTC and the primary plane. The daemon keeps an overlay
 * plane for itself and shows OSD messages there (volume and headphones in a
 * box centred near the top; external power plugged in or out, watched here
 * from power_supply uevents, as the battery in the top right, where the
 * launcher shows it); the display engine blends it over the app's image, so it costs the app
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
 * over whoever holds the lease: Exit game / System / Resume over an app; the
 * System page (straight away over the launcher) has Settings, Sleep, Reboot
 * and Shut down; Settings has a brightness slider (D-pad left/right; the
 * level is saved to the file given as argument and restored at start), the
 * performance overlay switch, USB video (the screen as a USB webcam,
 * /usr/libexec/s30/usb-video), the stick calibration and USB storage. The
 * battery (level, and a bolt while charging)
 * shows in a small box in the screen's top right, where the launcher shows it. That process is frozen (SIGSTOP) and
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
 *   "osd ICON PERCENT"       ICON: speaker, headphones, mute (box with a
 *                            bar), charging, battery (the battery, at
 *                            PERCENT, with a bolt for charging); PERCENT 0-100
 *
 * Volume changes while the menu is open show the volume box above it (eased
 * and faded the same way).
 *
 * Usage: s30-display [BRIGHTNESS_FILE]
 */
#define _GNU_SOURCE // struct ucred
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <math.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
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
#include <sys/wait.h>
#include <dirent.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <time.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "osd_button.h"
#include "osd_font.h"
#include "s30-frames.h"

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

/* The OSD plane outside the menu: the screen's top strip (the box and the
   battery), drawn in screen coordinates */
#define STRIP_H (BOX_Y + BOX_H)

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
static int sShowW = BUF_W, sShowH = STRIP_H; // part of the buffer the plane shows (landscape, from 0, 0)
static int sPlaneOn;          // overlay plane enabled (for the current lease)
static char sIcon[16] = "speaker";
static float sTarget, sShown; // bar percent: requested, drawn
static float sAlpha;          // box opacity drawn
static int sDirty;            // needs a redraw
static uint64_t sOsdUntil, sNextLeaseCheck;
static pid_t sAppPid;         // process holding the lease
static int sAppLauncher;      // ... and it is the launcher (no performance overlay)
static int sMenu, sMenuSel;   // system menu open, selected item
static int sMenuPage;         // 0: app, 1: system, 2: stick calibration, 3: settings, 4: USB confirmation
enum { PERF_OFF, PERF_FULL, PERF_FPS, PERF_MODES };
static int sPerf;             // performance overlay (System > Overlay, saved)
static char sPerfFile[320];   // where sPerf is saved ("" if nowhere)
static uint64_t sPerfNext;    // next overlay update (0: now)
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
		const unsigned char* g = kOsdFont[(*text < 0x20 || *text > 0x7f ? '?' : *text) - 0x20];
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

	percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	draw_box(ox, oy, BOX_W, BOX_H, 0.75f * alpha);
	draw_icon(strcmp(icon, "headphones") == 0 ? kHeadphones
	          : strcmp(icon, "mute") == 0     ? kMute
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

/* Battery at `capacity` in a small box in the screen's top right, where the
   launcher shows it (right edge 10 px in, centred 16 px down): a battery
   filled to the level (green, yellow, red) with the percentage inside, and
   a bolt to its left while `charging` (the battery itself never moves) */
static void draw_battery(int capacity, int charging, float alpha) {
	const int bw = 42, bh = 18, right = PANEL_H - 10, y = 16 - bh / 2, pad = 5;
	int len, x, left, i, j;
	char text[8];
	uint32_t level;

	if (capacity < 0) {
		return; // no fuel gauge
	}
	capacity = capacity > 100 ? 100 : capacity;
	level = capacity > 50 ? 0x46c85a : capacity > 20 ? 0xebbe28 : 0xe13c32;
	x = right - 3 - bw;
	left = charging ? x - 4 - 10 : x;
	draw_box(left - pad, y - pad, right - left + 2 * pad, bh + 2 * pad, 0.92f * alpha);
	// Outline only: the empty part shows the box, as translucent as the menu
	fill(x, y, bw, 1, argb(0xc8c8c8, alpha));
	fill(x, y + bh - 1, bw, 1, argb(0xc8c8c8, alpha));
	fill(x, y + 1, 1, bh - 2, argb(0xc8c8c8, alpha));
	fill(x + bw - 1, y + 1, 1, bh - 2, argb(0xc8c8c8, alpha));
	fill(x + 2, y + 2, (bw - 4) * capacity / 100, bh - 4, argb(level, alpha));
	fill(x + bw, y + 5, 3, bh - 10, argb(0xc8c8c8, alpha)); // nub
	// Percentage inside, shadowed (the digits are font rows 2-11: centred)
	len = snprintf(text, sizeof(text), "%d%%", capacity);
	draw_text(text, x + (bw - len * 8) / 2 + 1, y + bh / 2 - 7 + 1, 1, argb(0x000000, alpha));
	draw_text(text, x + (bw - len * 8) / 2, y + bh / 2 - 7, 1, argb(0xffffff, alpha));
	if (charging) {
		for (j = 0; j < 7; j++) {
			for (i = 0; i < 5; i++) {
				if (kBolt[j][i] == '#') {
					fill(left + i * 2, y + 2 + j * 2, 2, 2, argb(0xffffff, alpha));
				}
			}
		}
	}
}

static int external_power(int* capacity, int* charging);

/* The OSD shows the battery (sTarget is its level) rather than the box */
static int osd_is_battery(void) {
	return strcmp(sIcon, "battery") == 0 || strcmp(sIcon, "charging") == 0;
}

/* In an app the menu opens on the app page, whose System row opens the
   system page; over the launcher it opens on the system page, where the last
   row closes the menu instead of going back. The system page: Settings,
   sleep, reboot, power off. Settings (page 3): the brightness slider (NULL),
   the performance overlay switch, USB video, the stick calibration (its own
   screen, page 2), USB storage (confirmed on page 4). */
enum { APP_EXIT, APP_SCREENSHOT, APP_SYSTEM, APP_RESUME };
static const char* const kAppItems[] = { "Exit game", "Screenshot", "System", "Resume" };
enum { SYS_SETTINGS, SYS_SLEEP, SYS_REBOOT, SYS_POWEROFF, SYS_BACK };
static const char* const kSysItems[] = { "Settings", "Sleep", "Reboot", "Shut down", "Back" };
enum { SET_BRIGHTNESS, SET_PERF, SET_USB_VIDEO, SET_CALIBRATE, SET_USB, SET_BACK };
static const char* const kSetItems[] = { NULL, "Overlay", "USB video", "Calibrate stick", "USB storage", "Back" };
/* USB storage confirmation: two text rows (USB_TEXT_ROWS, not selectable), OK, Cancel */
enum { USB_TEXT_ROWS = 2, USB_OK = 2, USB_CANCEL };
static const char* const kUsbItems[] = { "USB storage mode", "reboots the device", "OK", "Cancel" };

static const char* const* menu_items(void) {
	return sMenuPage == 4 ? kUsbItems : sMenuPage == 3 ? kSetItems : sMenuPage == 1 ? kSysItems : kAppItems;
}

static int menu_count(void) {
	return sMenuPage == 0 || sMenuPage == 4 ? 4 : sMenuPage == 3 ? 6 : 5;
}

/* The row B returns from (its glyph is drawn there) */
static int menu_back_row(void) {
	return sMenuPage == 4 ? USB_CANCEL : sMenuPage == 3 ? SET_BACK : sMenuPage == 1 ? SYS_BACK : APP_RESUME;
}

/* USB video is on while the UVC function is in the gadget's configuration
   (usb-video owns the switch and saves it) */
static int usb_video_on(void) {
	return access("/sys/kernel/config/usb_gadget/s30/configs/c.1/uvc.usb0", F_OK) == 0;
}

static int on_brightness_row(void) {
	return sMenuPage == 3 && sMenuSel == SET_BRIGHTNESS;
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

static void blend(int x, int y, uint32_t rgb, float a);

/* A face button (osd_button.h): the anti-aliased disc, the letter cut in dark */
static void draw_button(int x0, int y0, const char* const* letter) {
	int x, y;

	for (y = 0; y < BUTTON_SIZE; y++) {
		for (x = 0; x < BUTTON_SIZE; x++) {
			blend(x0 + x, y0 + y, button_letter(letter, x, y) ? BUTTON_DARK : BUTTON_LIGHT, button_coverage(x, y));
		}
	}
}

/* Top of menu row i */
static int menu_row_y(int i) {
	const int h = menu_count() * MENU_ROW_H + 2 * MENU_PAD;

	return (PANEL_W - h) / 2 + MENU_PAD + i * MENU_ROW_H;
}

static void draw_menu(void) {
	const char* const* items = menu_items();
	const int rowH = MENU_ROW_H, count = menu_count(), h = count * rowH + 2 * MENU_PAD;
	const int y0 = (PANEL_W - h) / 2, top = y0 + MENU_PAD;
	int i, capacity, charging;

	draw_box(MENU_X, y0, MENU_W, h, 0.92f);
	external_power(&capacity, &charging);
	draw_battery(capacity, charging, 1.0f);
	fill(BOX_X, BOX_Y, BOX_W, BOX_H, 0); // the OSD box above the menu, as osd_frame left it
	if (sAlpha > 0.0f && !osd_is_battery()) { // the menu shows the battery itself
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
			char label[32];
			int len, x;

			if (sMenuPage == 3 && i == SET_PERF) {
				static const char* const modes[] = { "Off", "Full", "FPS" };
				snprintf(label, sizeof(label), "%s: %s", items[i], modes[sPerf]);
			} else if (sMenuPage == 3 && i == SET_USB_VIDEO) {
				snprintf(label, sizeof(label), "%s: %s", items[i], usb_video_on() ? "On" : "Off");
			} else if (sMenuPage == 1 && i == SYS_BACK && sMenuLauncher) {
				snprintf(label, sizeof(label), "Close");
			} else {
				snprintf(label, sizeof(label), "%s", items[i]);
			}
			len = (int)strlen(label);
			x = MENU_X + (MENU_W - len * 8) / 2;
			draw_text(label, x, y + (rowH - 16) / 2, 1,
			          argb(sMenuPage == 4 && i < USB_TEXT_ROWS ? 0xc0c0c0 : 0xffffff, 1.0f));
			if (i == menu_back_row()) {
				draw_button(x - 6 - 14, y + (rowH - 14) / 2, kLetterB); // what B does
			}
		}
	}
}

static void present_synced(void);
static void menu_redraw(void);

/* ---------------------------------------------------------------------------
 * Stick calibration (System > Calibrate stick), drawn over the whole screen
 *
 * 1. Centre: the stick rests for CAL_REST_MS; its average is the zero point.
 * 2. Rotate: the user turns the stick around its edge (CAL_TURNS turns); the
 *    furthest point in each of CAL_BINS directions is kept. Done after the
 *    turns once 3/4 of the directions have been reached.
 * 3. Edges: the polygon through those points (the mesh the plot shows) gives
 *    how far the stick reaches in each of CAL_EDGES directions, wherever its
 *    shape: the driver scales each sample by 127 over the edge in its
 *    direction, so every direction reaches full deflection, as Dolphin does.
 * 4. Test: the corrected stick live; A saves, X redoes, B leaves it unchanged.
 *
 * With a calibration already in the driver, it opens on that one instead
 * (CAL_CURRENT): its edges as the mesh and the corrected stick live, as in the
 * test; X starts a new calibration, B goes back.
 *
 * The gamepad driver takes "cx cy m00 m01 m10 m11 deadzone e0 .. e31" (M,
 * the identity here, in Q12; edges in 1/16 counts from +x towards +y) from
 * sysfs; the result is also saved to CAL_FILE, which /etc/s30/rc loads at
 * boot. While calibrating, the driver is set to identity (raw values). The
 * rest point moves a few counts after the stick has been moved (about 5 on
 * the test unit), so the deadzone is wider than that. The plot shows the mesh
 * and the raw stick; the test adds the corrected stick, which should reach
 * the ring evenly all round.
 * ------------------------------------------------------------------------- */

#define CAL_FILE "/mnt/sd/home/.config/joystick.cal"
#define CAL_SYSFS_GLOB "/sys/bus/i2c/drivers/pocketgo-s30-gamepad/*/calibration"
#define CAL_IDENTITY "128 128 4096 0 0 4096 0" // no deadzone: raw values
#define CAL_DEADZONE 10 // out of 127
#define CAL_EDGES 32 // as the driver's table
#define CAL_BINS 64
#define CAL_TURNS 3.0
#define CAL_REST_MS 1000
#define CAL_MIN_R 20 // raw counts from the centre: a direction counts as reached
#define CAL_LOG "/tmp/s30-joycal.log" // the last calibration's raw samples, for diagnosis

/* Plot: the stick's full range (+-128) over radius CAL_R around (CAL_X, CAL_Y) */
#define CAL_X 150
#define CAL_Y 172
#define CAL_R 118
#define CAL_TEXT_X 300

#define CAL_BG 0x101018
#define CAL_DISC 0x181826
#define CAL_GRID 0x262638
#define CAL_RING 0x484866
#define CAL_ACCENT 0x8060e0
#define CAL_DIM 0x8888a0
#define CAL_EDGE 0xb4a0ff // the mesh's outline
#define CAL_FIXED 0x60e0a0 // the corrected stick's dot
#define CAL_DEADZONE_RGB 0xe08060

enum { CAL_CENTER, CAL_ROTATE, CAL_TEST, CAL_CURRENT };

static struct {
	int phase;
	char sysfs[256], previous[512];
	FILE* log; // CAL_LOG
	uint64_t start;
	int rx, ry; // raw stick, 0-255
	double cx, cy; // centre (raw)
	uint64_t restSince; // centre: start of the current still window
	double sumX, sumY;
	int samples, minX, maxX, minY, maxY;
	double binR[CAL_BINS], binX[CAL_BINS], binY[CAL_BINS]; // edge per direction, relative to the centre
	double turns, lastAng;
	int haveAng, filled;
	int edge[CAL_EDGES]; // reach per direction, 1/16 counts, as the driver takes it
} sCal;

static uint32_t sCalBg[BUF_W * BUF_H]; // the plot's static parts, as in sBack

static int cal_write(const char* path, const char* text) {
	FILE* f = fopen(path, "w");
	int ok;

	if (f == NULL) {
		return 0;
	}
	ok = fputs(text, f) >= 0;
	return fclose(f) == 0 && ok;
}

/* How far the polygon through the reached directions' edge points goes from
   the centre in direction `a` (radians); 0 if it doesn't cross it */
static double cal_reach(double a) {
	const double ux = cos(a), uy = sin(a);
	double px[CAL_BINS], py[CAL_BINS], best = 0;
	int n = 0, i;

	for (i = 0; i < CAL_BINS; i++) {
		if (sCal.binR[i] > CAL_MIN_R) {
			px[n] = sCal.binX[i];
			py[n] = sCal.binY[i];
			n++;
		}
	}
	for (i = 0; i < n; i++) { // the ray against each side
		double ex = px[(i + 1) % n] - px[i], ey = py[(i + 1) % n] - py[i];
		double den = ux * ey - uy * ex, t, s;

		if (fabs(den) < 1e-9) {
			continue;
		}
		t = (px[i] * ey - py[i] * ex) / den;   // along the ray
		s = (px[i] * uy - py[i] * ux) / den;   // along the side
		if (t > best && s >= 0 && s <= 1) {
			best = t;
		}
	}
	return best;
}

/* The stick's reach in direction `a` (radians), in counts, from sCal.edge
   (interpolated as the driver does) */
static double cal_edge_at(double a) {
	double pos = (a < 0 ? a + 2 * M_PI : a) / (2 * M_PI) * CAL_EDGES, f = pos - floor(pos);
	int i = (int)pos % CAL_EDGES;

	return (sCal.edge[i] * (1 - f) + sCal.edge[(i + 1) % CAL_EDGES] * f) / 16;
}

/* The driver's output for raw (x, y), with the edges found (sCal.edge) */
static void cal_corrected(int x, int y, double out[2]) {
	const double dx = x - sCal.cx, dy = y - sCal.cy, a = atan2(dy, dx);
	double e = cal_edge_at(a);
	double ox = dx * 127 / e, oy = dy * 127 / e, r = sqrt(ox * ox + oy * oy);

	if (r <= CAL_DEADZONE) {
		ox = oy = 0;
	} else { // rescaled past the deadzone, as the driver
		ox *= (r - CAL_DEADZONE) * 127 / (127 - CAL_DEADZONE) / r;
		oy *= (r - CAL_DEADZONE) * 127 / (127 - CAL_DEADZONE) / r;
	}
	out[0] = ox < -128 ? -128 : ox > 127 ? 127 : ox;
	out[1] = oy < -128 ? -128 : oy > 127 ? 127 : oy;
}

/* Back to step 1 */
static void cal_restart(void) {
	memset(&sCal.restSince, 0, sizeof(sCal) - offsetof(typeof(sCal), restSince));
	sCal.phase = CAL_CENTER;
	sCal.cx = sCal.cy = 128;
	sCal.minX = sCal.minY = 255;
}

/* Loads the driver's calibration (`text`, as sysfs shows it) into sCal for
   showing, the edges as the mesh's points. Returns 0 if it has no edges (not
   calibrated). */
static int cal_load(const char* text) {
	int v[7 + CAL_EDGES], n, used, i;

	for (n = 0; n < 7 + CAL_EDGES && sscanf(text, "%d%n", &v[n], &used) == 1; n++) {
		text += used;
	}
	if (n != 7 + CAL_EDGES) {
		return 0;
	}
	cal_restart();
	sCal.cx = v[0];
	sCal.cy = v[1];
	memcpy(sCal.edge, &v[7], sizeof(sCal.edge));
	for (i = 0; i < CAL_BINS; i++) { // each bin's middle direction, as cal_update bins them
		double a = (i + 0.5) * 2 * M_PI / CAL_BINS - M_PI, e = cal_edge_at(a);

		sCal.binR[i] = e;
		sCal.binX[i] = e * cos(a);
		sCal.binY[i] = e * sin(a);
	}
	sCal.phase = CAL_CURRENT;
	return 1;
}

/* --- Drawing: anti-aliased, blended over what sBack holds (opaque here) --- */

static void blend(int x, int y, uint32_t rgb, float a) {
	uint32_t* p;

	if (a <= 0.0f || x < 0 || x >= BUF_W || y < 0 || y >= BUF_H) {
		return;
	}
	p = &sBack[(BUF_W - 1 - x) * BUF_H + y];
	*p = (*p & 0xff000000) | mix(rgb, *p & 0xffffff, a > 1.0f ? 1.0f : a);
}

static float clamp01(float v) {
	return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

/* Disc of radius r, or a ring of width w around radius r if w > 0 */
static void cal_circle(float cx, float cy, float r, float w, uint32_t rgb, float a) {
	const float out = r + (w > 0 ? w / 2 : 0) + 1;
	int x, y;

	for (y = (int)(cy - out); y <= (int)(cy + out) + 1; y++) {
		for (x = (int)(cx - out); x <= (int)(cx + out) + 1; x++) {
			float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
			blend(x, y, rgb, a * clamp01(w > 0 ? w / 2 + 0.5f - fabsf(d - r) : r + 0.5f - d));
		}
	}
}

/* Stick offset from the centre (counts) to the plot */
static void cal_plot(double dx, double dy, float out[2]) {
	out[0] = CAL_X + (float)(dx * CAL_R / 128);
	out[1] = CAL_Y + (float)(dy * CAL_R / 128);
}

/* One-pixel line, Xiaolin Wu's */
static void cal_line(float x0, float y0, float x1, float y1, uint32_t rgb, float a) {
	int steep = fabsf(y1 - y0) > fabsf(x1 - x0);
	float t, g;
	int x;

	if (steep) {
		t = x0, x0 = y0, y0 = t;
		t = x1, x1 = y1, y1 = t;
	}
	if (x0 > x1) {
		t = x0, x0 = x1, x1 = t;
		t = y0, y0 = y1, y1 = t;
	}
	g = x1 - x0 < 1e-3f ? 0.0f : (y1 - y0) / (x1 - x0);
	for (x = (int)lroundf(x0); x <= (int)lroundf(x1); x++) {
		float y = y0 + g * (x - x0), f = y - floorf(y);
		int yi = (int)floorf(y);

		if (steep) {
			blend(yi, x, rgb, a * (1 - f));
			blend(yi + 1, x, rgb, a * f);
		} else {
			blend(x, yi, rgb, a * (1 - f));
			blend(x, yi + 1, rgb, a * f);
		}
	}
}

/* The edge mesh, centred on the plot: the polygon through the furthest point
   reached in each direction, filled with 4x4 coverage per pixel, outlined.
   Directions not reached for more than 1/8 turn go through the centre. */
static void cal_mesh(uint32_t rgb, float a, uint32_t edgeRgb) {
	enum { SUB = 4, MAXV = CAL_BINS * 2 };
	static float cov[CAL_R * 2 + 16][CAL_R * 2 + 16]; // around the plot centre
	const int ox = CAL_X - CAL_R - 8, oy = CAL_Y - CAL_R - 8, size = CAL_R * 2 + 16;
	float vx[MAXV], vy[MAXV], c[2];
	int n = 0, i, gap = 0, first = -1, sy, x, y;

	cal_plot(0, 0, c);
	for (i = 0; i < CAL_BINS; i++) {
		if (sCal.binR[i] > CAL_MIN_R) {
			first = first < 0 ? i : first;
		}
	}
	if (first < 0) {
		return;
	}
	for (i = 0; i < CAL_BINS; i++) {
		int b = (first + i) % CAL_BINS;
		float p[2];

		if (sCal.binR[b] <= CAL_MIN_R) {
			gap++;
			continue;
		}
		if (gap > CAL_BINS / 8) {
			vx[n] = c[0], vy[n] = c[1], n++;
		}
		gap = 0;
		cal_plot(sCal.binX[b], sCal.binY[b], p);
		vx[n] = p[0], vy[n] = p[1], n++;
	}
	if (gap + first > CAL_BINS / 8) { // the wrap back to the first point
		vx[n] = c[0], vy[n] = c[1], n++;
	}
	if (n < 3) {
		return;
	}
	memset(cov, 0, sizeof(cov));
	// Each sub-row: the even-odd spans between edge crossings, added to the
	// pixels they cover (fractional at the ends)
	for (sy = 0; sy < size * SUB; sy++) {
		const float py = oy + (sy + 0.5f) / SUB;
		float xs[MAXV];
		int nx = 0, j;

		for (i = 0; i < n; i++) {
			j = (i + 1) % n;
			if ((vy[i] <= py) != (vy[j] <= py)) {
				xs[nx++] = vx[i] + (py - vy[i]) / (vy[j] - vy[i]) * (vx[j] - vx[i]) - ox;
			}
		}
		for (i = 1; i < nx; i++) { // insertion sort: few crossings
			float t = xs[i];
			for (j = i; j > 0 && xs[j - 1] > t; j--) {
				xs[j] = xs[j - 1];
			}
			xs[j] = t;
		}
		for (i = 0; i + 1 < nx; i += 2) {
			float x0 = xs[i] < 0 ? 0 : xs[i], x1 = xs[i + 1] > size ? size : xs[i + 1];

			for (x = (int)x0; x < size && x < x1; x++) {
				float l = x0 > x ? x0 : x, r = x1 < x + 1 ? x1 : x + 1;
				cov[sy / SUB][x] += (r - l) / SUB;
			}
		}
	}
	for (y = 0; y < size; y++) {
		for (x = 0; x < size; x++) {
			if (cov[y][x] > 0) {
				blend(ox + x, oy + y, rgb, a * clamp01(cov[y][x]));
			}
		}
	}
	for (i = 0; i < n; i++) {
		cal_line(vx[i], vy[i], vx[(i + 1) % n], vy[(i + 1) % n], edgeRgb, 1.0f);
	}
}

/* "[button] label" at (x, y); returns the x after it */
static int cal_hint(int x, int y, const char* const* letter, const char* label) {
	draw_button(x, y + 1, letter);
	draw_text(label, x + 14 + 6, y, 1, argb(0xffffff, 1.0f));
	return x + 14 + 6 + (int)strlen(label) * 8 + 16;
}

/* Horizontal progress bar, like the brightness slider's */
static void cal_bar(int x, int y, int w, float f) {
	fill(x, y, w, 6, argb(0x2c2c40, 1.0f));
	fill(x, y, (int)(w * clamp01(f)), 6, argb(CAL_ACCENT, 1.0f));
}

/* The static parts: background, plot disc and its full-deflection ring, title */
static void cal_draw_bg(void) {
	fill(0, 0, BUF_W, BUF_H, argb(CAL_BG, 1.0f));
	cal_circle(CAL_X, CAL_Y, CAL_R + 6, 0, CAL_DISC, 1.0f);
	cal_circle(CAL_X, CAL_Y, CAL_R * 127 / 128.0f, 1.5f, CAL_RING, 1.0f);
	// Cross through the centre, out to the ring
	cal_line(CAL_X - CAL_R, CAL_Y, CAL_X + CAL_R, CAL_Y, CAL_GRID, 1.0f);
	cal_line(CAL_X, CAL_Y - CAL_R, CAL_X, CAL_Y + CAL_R, CAL_GRID, 1.0f);
	draw_text("Stick calibration", CAL_TEXT_X, 24, 1, argb(0xffffff, 1.0f));
	fill(CAL_TEXT_X, 46, BUF_W - CAL_TEXT_X - 16, 1, argb(CAL_GRID, 1.0f));
	memcpy(sCalBg, sBack, sizeof(sCalBg));
}

/* Steps, the current one's instructions, the buttons */
static void cal_draw_text(uint64_t now) {
	static const char* const kSteps[3] = { "Centre", "Rotate", "Test" };
	const uint32_t white = argb(0xffffff, 1.0f), dim = argb(CAL_DIM, 1.0f);
	const int w = BUF_W - CAL_TEXT_X - 16;
	char line[32];
	int i, y = 60, x;

	if (sCal.phase == CAL_CURRENT) {
		draw_text("Current", CAL_TEXT_X, y, 1, white);
		y += 36;
		draw_text("The stick as it", CAL_TEXT_X, y, 1, white);
		draw_text("is calibrated", CAL_TEXT_X, y + 18, 1, white);
		cal_circle(CAL_TEXT_X + 5, y + 54, 4.5f, 0, CAL_FIXED, 1.0f);
		draw_text("corrected", CAL_TEXT_X + 18, y + 46, 1, dim);
		cal_circle(CAL_TEXT_X + 5, y + 74, 4.5f, 0, 0xffffff, 1.0f);
		draw_text("raw", CAL_TEXT_X + 18, y + 66, 1, dim);
		cal_hint(CAL_TEXT_X, BUF_H - 52, kLetterX, "Recalibrate");
		cal_hint(CAL_TEXT_X, BUF_H - 30, kLetterB, "Back");
		return;
	}
	for (i = 0; i < 3; i++, y += 22) {
		snprintf(line, sizeof(line), "%d  %s", i + 1, kSteps[i]);
		if (i == sCal.phase) {
			fill(CAL_TEXT_X - 6, y - 3, w + 12, 22, argb(CAL_ACCENT, 1.0f));
		}
		draw_text(line, CAL_TEXT_X, y, 1, i <= sCal.phase ? white : dim);
	}
	y += 14;
	if (sCal.phase == CAL_CENTER) {
		draw_text("Let go of the", CAL_TEXT_X, y, 1, white);
		draw_text("stick", CAL_TEXT_X, y + 18, 1, white);
		cal_bar(CAL_TEXT_X, y + 46, w,
		        sCal.restSince != 0 ? (float)(now - sCal.restSince) / CAL_REST_MS : 0.0f);
	} else if (sCal.phase == CAL_ROTATE) {
		draw_text("Roll the stick", CAL_TEXT_X, y, 1, white);
		draw_text("around its edge", CAL_TEXT_X, y + 18, 1, white);
		snprintf(line, sizeof(line), "Turns %.1f / %.0f", fabs(sCal.turns) < CAL_TURNS ? fabs(sCal.turns) : CAL_TURNS,
		         CAL_TURNS);
		draw_text(line, CAL_TEXT_X, y + 46, 1, dim);
		cal_bar(CAL_TEXT_X, y + 66, w, (float)(fabs(sCal.turns) / CAL_TURNS));
		snprintf(line, sizeof(line), "Directions %d%%", sCal.filled * 100 / CAL_BINS);
		draw_text(line, CAL_TEXT_X, y + 82, 1, dim);
		cal_bar(CAL_TEXT_X, y + 102, w, (float)sCal.filled / (CAL_BINS * 3 / 4));
	} else {
		draw_text("The stick should", CAL_TEXT_X, y, 1, white);
		draw_text("reach the circle", CAL_TEXT_X, y + 18, 1, white);
		draw_text("evenly all round", CAL_TEXT_X, y + 36, 1, white);
		cal_circle(CAL_TEXT_X + 5, y + 72, 4.5f, 0, CAL_FIXED, 1.0f);
		draw_text("corrected", CAL_TEXT_X + 18, y + 64, 1, dim);
		cal_circle(CAL_TEXT_X + 5, y + 92, 4.5f, 0, 0xffffff, 1.0f);
		draw_text("raw", CAL_TEXT_X + 18, y + 84, 1, dim);
	}
	x = CAL_TEXT_X;
	if (sCal.phase == CAL_TEST) {
		x = cal_hint(x, BUF_H - 52, kLetterA, "Save");
		cal_hint(x, BUF_H - 52, kLetterX, "Redo");
	}
	cal_hint(CAL_TEXT_X, BUF_H - 30, kLetterB, "Cancel");
}

/* The volume box over the calibration: drawn as over the menu, then its
   translucent pixels blended over the calibration under them, so the frozen
   app doesn't show through */
static void cal_draw_osd(void) {
	static uint32_t under[BOX_W * BOX_H];
	int x, y;

	for (y = 0; y < BOX_H; y++) {
		for (x = 0; x < BOX_W; x++) {
			under[y * BOX_W + x] = sBack[(BUF_W - 1 - (BOX_X + x)) * BUF_H + BOX_Y + y];
		}
	}
	draw_osd(BOX_X, BOX_Y, sIcon, sShown, sAlpha);
	for (y = 0; y < BOX_H; y++) {
		for (x = 0; x < BOX_W; x++) {
			uint32_t* p = &sBack[(BUF_W - 1 - (BOX_X + x)) * BUF_H + BOX_Y + y];
			uint32_t b = under[y * BOX_W + x];

			*p = (b & 0xff000000) | mix(*p & 0xffffff, b & 0xffffff, (*p >> 24) / 255.0f);
		}
	}
}

static void cal_draw(uint64_t now) {
	float p[2];

	memcpy(sBack, sCalBg, sizeof(sBack));
	if (sCal.phase != CAL_CENTER) {
		cal_mesh(CAL_ACCENT, 0.45f, CAL_EDGE);
	}
	// The deadzone, in the corrected stick's scale (where the driver applies it)
	cal_circle(CAL_X, CAL_Y, CAL_DEADZONE * CAL_R / 128.0f, 0, CAL_DEADZONE_RGB, 0.35f);
	cal_circle(CAL_X, CAL_Y, CAL_DEADZONE * CAL_R / 128.0f, 1.0f, CAL_DEADZONE_RGB, 1.0f);
	cal_plot(sCal.rx - sCal.cx, sCal.ry - sCal.cy, p); // raw, around the centre found (128 until then)
	cal_circle(p[0], p[1], 4.5f, 0, 0xffffff, 1.0f);
	if (sCal.phase >= CAL_TEST) { // corrected, as the driver will report it
		double o[2];

		cal_corrected(sCal.rx, sCal.ry, o);
		cal_plot(o[0], o[1], p);
		cal_circle(p[0], p[1], 4.5f, 0, CAL_FIXED, 1.0f);
	}
	cal_draw_text(now);
	if (sAlpha > 0.0f && !osd_is_battery()) { // volume changes, as over the menu
		cal_draw_osd();
	}
}

/* --- Steps --- */

static void cal_update(uint64_t now) {
	const double dx = sCal.rx - sCal.cx, dy = sCal.ry - sCal.cy, r = sqrt(dx * dx + dy * dy), ang = atan2(dy, dx);
	int i;

	if (sCal.log != NULL) { // ms, step, raw x, raw y, centre
		fprintf(sCal.log, "%llu %d %d %d %.1f %.1f\n", (unsigned long long)(now - sCal.start), sCal.phase, sCal.rx,
		        sCal.ry, sCal.cx, sCal.cy);
	}
	if (sCal.phase == CAL_CENTER) {
		// CAL_REST_MS of samples within a few counts of each other
		sCal.minX = sCal.rx < sCal.minX ? sCal.rx : sCal.minX;
		sCal.maxX = sCal.rx > sCal.maxX ? sCal.rx : sCal.maxX;
		sCal.minY = sCal.ry < sCal.minY ? sCal.ry : sCal.minY;
		sCal.maxY = sCal.ry > sCal.maxY ? sCal.ry : sCal.maxY;
		if (sCal.maxX - sCal.minX > 4 || sCal.maxY - sCal.minY > 4 || sCal.restSince == 0) {
			sCal.sumX = sCal.sumY = 0;
			sCal.samples = 0;
			sCal.minX = sCal.maxX = sCal.rx;
			sCal.minY = sCal.maxY = sCal.ry;
			sCal.restSince = now;
		}
		sCal.sumX += sCal.rx;
		sCal.sumY += sCal.ry;
		sCal.samples++;
		if (now - sCal.restSince >= CAL_REST_MS) {
			sCal.cx = lround(sCal.sumX / sCal.samples); // whole counts, as the driver takes it
			sCal.cy = lround(sCal.sumY / sCal.samples);
			sCal.phase = CAL_ROTATE;
		}
	} else if (sCal.phase == CAL_ROTATE) {
		int b = (int)((ang + M_PI) / (2 * M_PI) * CAL_BINS) % CAL_BINS;
		double maxR = 0;

		for (i = 0; i < CAL_BINS; i++) {
			maxR = sCal.binR[i] > maxR ? sCal.binR[i] : maxR;
		}
		if (r > sCal.binR[b]) {
			sCal.binR[b] = r;
			sCal.binX[b] = dx;
			sCal.binY[b] = dy;
		}
		// Count turns made near the edge
		if (r > 0.6 * maxR && r > CAL_MIN_R) {
			if (sCal.haveAng) {
				double d = ang - sCal.lastAng;
				d = d > M_PI ? d - 2 * M_PI : d < -M_PI ? d + 2 * M_PI : d;
				sCal.turns += d / (2 * M_PI);
			}
			sCal.lastAng = ang;
			sCal.haveAng = 1;
		} else {
			sCal.haveAng = 0;
		}
		sCal.filled = 0;
		for (i = 0; i < CAL_BINS; i++) {
			sCal.filled += sCal.binR[i] > CAL_MIN_R;
		}
		if (sCal.filled >= CAL_BINS * 3 / 4 && fabs(sCal.turns) >= CAL_TURNS) {
			for (i = 0; i < CAL_EDGES; i++) {
				double e = cal_reach(i * 2 * M_PI / CAL_EDGES) * 16;
				sCal.edge[i] = e < 16 ? 16 : e > 256 * 16 ? 256 * 16 : (int)lround(e);
			}
			sCal.phase = CAL_TEST;
		}
	}
}

/* Opens the calibration from the system page: the driver to raw values */
static void cal_open(void) {
	struct input_absinfo ax, ay;
	glob_t g;
	FILE* f;

	if (sPadFd < 0 || glob(CAL_SYSFS_GLOB, 0, NULL, &g) != 0) {
		fprintf(stderr, "s30-display: calibration: no gamepad calibration attribute\n");
		return;
	}
	snprintf(sCal.sysfs, sizeof(sCal.sysfs), "%s", g.gl_pathv[0]);
	globfree(&g);
	sCal.previous[0] = '\0';
	f = fopen(sCal.sysfs, "r");
	if (f != NULL) {
		if (fgets(sCal.previous, sizeof(sCal.previous), f) == NULL) {
			sCal.previous[0] = '\0';
		}
		fclose(f);
	}
	cal_write(sCal.sysfs, CAL_IDENTITY);
	sCal.rx = ioctl(sPadFd, EVIOCGABS(ABS_X), &ax) == 0 ? ax.value : 128;
	sCal.ry = ioctl(sPadFd, EVIOCGABS(ABS_Y), &ay) == 0 ? ay.value : 128;
	if (!cal_load(sCal.previous)) {
		cal_restart();
	}
	sCal.log = fopen(CAL_LOG, "w");
	sCal.start = now_ms();
	sMenuPage = 2;
	cal_draw_bg();
}

/* Leaves it: `save` the fit (test step), else the driver as it was */
static void cal_close(int save) {
	char text[512];
	int len, i;

	len = snprintf(text, sizeof(text), "%d %d 4096 0 0 4096 %d", (int)lround(sCal.cx), (int)lround(sCal.cy),
	               CAL_DEADZONE);
	for (i = 0; i < CAL_EDGES; i++) {
		len += snprintf(text + len, sizeof(text) - len, " %d", sCal.edge[i]);
	}
	snprintf(text + len, sizeof(text) - len, "\n");
	if (save) {
		save = cal_write(sCal.sysfs, text) && cal_write(CAL_FILE, text);
		printf("s30-display: calibration %s: %s", save ? "saved" : "save FAILED", text);
	}
	if (!save) {
		cal_write(sCal.sysfs, sCal.previous[0] != '\0' ? sCal.previous : "128 128 4096 0 0 4096");
	}
	if (sCal.log != NULL) {
		fprintf(sCal.log, "# %s %s", save ? "saved" : "not saved", text);
		fclose(sCal.log);
		sCal.log = NULL;
	}
	sMenuPage = 3; // back to Settings
}

static void cal_input(const struct input_event* ev) {
	if (ev->type == EV_ABS) {
		if (ev->code == ABS_X) {
			sCal.rx = ev->value;
		} else if (ev->code == ABS_Y) {
			sCal.ry = ev->value;
		}
	} else if (ev->type == EV_KEY && ev->value == 1) {
		if (ev->code == BTN_SOUTH) { // B: cancel
			cal_close(0);
			menu_redraw();
		} else if (ev->code == BTN_EAST && sCal.phase == CAL_TEST) { // A: save
			cal_close(1);
			menu_redraw();
		} else if (ev->code == BTN_NORTH && sCal.phase >= CAL_TEST) { // X: redo, or recalibrate
			cal_restart();
		}
	}
}

/* A frame while calibrating (paced by the vblank); whether it drew */
static int cal_frame(void) {
	uint64_t now;

	if (!sMenu || sMenuPage != 2) {
		return 0;
	}
	now = now_ms();
	cal_update(now);
	cal_draw(now);
	present_synced();
	return 1;
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

/* Plane height outside the menu: the OSD strip, or the whole screen while
   the performance overlay is on */
static int plane_h(void) {
	return sPerf == PERF_FULL && !sAppLauncher ? BUF_H : STRIP_H;
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
		fill(0, 0, BUF_W, BUF_H, 0); // starts transparent, fades in
		present_rect(0, 0, BUF_W, BUF_H);
		set_plane(0, 0, BUF_W, plane_h());
		sPerfNext = 0;
	}
}

static int perf_visible(void);
static void perf_paint(uint64_t nowMs);

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
	if (sMenu && sMenuPage == 2) {
		// The calibration redraws every frame: it draws the box itself
	} else if (sMenu) {
		// Above the menu (the plane is the menu's, the whole screen): redraw
		// and copy in only the box's spot
		fill(BOX_X, BOX_Y, BOX_W, BOX_H, 0);
		if (sAlpha > 0.0f && !osd_is_battery()) { // the menu shows the battery itself
			draw_osd(BOX_X, BOX_Y, sIcon, sShown, sAlpha);
		}
		wait_vblank();
		present_rect(BOX_X, BOX_Y, BOX_W, BOX_H);
	} else {
		fill(0, 0, BUF_W, STRIP_H, 0);
		if (perf_visible()) {
			perf_paint(now_ms()); // the overlay's top, under the OSD
		}
		if (sAlpha > 0.0f && osd_is_battery()) {
			draw_battery((int)sTarget, strcmp(sIcon, "charging") == 0, sAlpha);
		} else if (sAlpha > 0.0f) {
			draw_osd(BOX_X, BOX_Y, sIcon, sShown, sAlpha);
		}
		wait_vblank();
		present_rect(0, 0, BUF_W, STRIP_H); // the strip only (the plane may be the whole screen)
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
	sMenuPage = sMenuLauncher; // the launcher has no app page
	sMenuSel = sMenuLauncher ? SYS_BACK : APP_RESUME;
	sAlpha = 0.0f;
	sOsdUntil = 0;
	// Move the plane only while it is transparent, then draw the menu: the
	// plane still sits where the OSD strip was until the move takes effect
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
	if (sMenuPage == 2) {
		cal_close(0); // the driver back as it was
	}
	sMenu = 0;
	sAlpha = 0.0f; // the OSD box goes with the menu
	sOsdUntil = 0;
	sRepeatDir = 0;
	if (sLessee != 0) {
		// Back to the (transparent) OSD strip, or the whole screen for the
		// performance overlay, before the app runs again
		fill(0, 0, BUF_W, BUF_H, 0);
		present_synced();
		set_plane(0, 0, BUF_W, plane_h());
		sPerfNext = 0; // overlay back at once
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

/* The game's picture (its primary plane, without the OSD or the menu) as
   /mnt/sd/screenshots/<app>-NNNN.png, numbered after the highest one there
   (no RTC: the clock restarts at 1970 every boot). The app is stopped under
   the menu, so the plane still shows its last frame. */
static void take_screenshot(void) {
	static const char kDir[] = "/mnt/sd/screenshots";
	char app[32] = "app", path[128], comm[64];
	int next = 1;
	size_t appLen;
	DIR* d;
	FILE* f;
	pid_t pid;

	snprintf(comm, sizeof(comm), "/proc/%d/comm", (int)sAppPid);
	f = fopen(comm, "r");
	if (f != NULL) {
		if (fgets(app, sizeof(app), f) != NULL) {
			app[strcspn(app, "\n")] = 0;
		}
		fclose(f);
	}
	appLen = strlen(app);
	mkdir(kDir, 0755);
	d = opendir(kDir);
	if (d != NULL) {
		struct dirent* e;

		while ((e = readdir(d)) != NULL) {
			int n;

			if (strncmp(e->d_name, app, appLen) == 0 && e->d_name[appLen] == '-' &&
			    sscanf(e->d_name + appLen + 1, "%d.png", &n) == 1 && n >= next) {
				next = n + 1;
			}
		}
		closedir(d);
	}
	snprintf(path, sizeof(path), "%s/%s-%04d.png", kDir, app, next);
	pid = fork();
	if (pid == 0) {
		execlp("s30-screenshot", "s30-screenshot", "-g", path, (char*)NULL);
		_exit(127);
	}
	if (pid > 0) {
		waitpid(pid, NULL, 0);
	}
	fprintf(stderr, "s30-display: screenshot %s\n", path);
}

/* Reboot or power off through init (which stops everything and syncs) */
static void run_system(const char* cmd) {
	pid_t pid = fork();

	if (pid == 0) {
		execlp(cmd, cmd, (char*)NULL);
		_exit(127);
	}
}

/* The marker s30-loader looks for at the next boot (see USB_MODE_FLAG there) */
static int usb_mode_flag(void) {
	int fd = open("/mnt/sd/boot/usb-mode", O_WRONLY | O_CREAT | O_CLOEXEC, 0644);

	if (fd < 0) {
		fprintf(stderr, "s30-display: usb-mode flag: %s\n", strerror(errno));
		return -1;
	}
	close(fd);
	sync();
	return 0;
}

/* USB video on or off; waits, as the label shows the result. The gadget
   rebinds, which takes well under a second. */
static void usb_video_toggle(void) {
	pid_t pid = fork();

	if (pid == 0) {
		execl("/usr/libexec/s30/usb-video", "usb-video", usb_video_on() ? "off" : "on", (char*)NULL);
		_exit(127);
	}
	if (pid > 0) {
		waitpid(pid, NULL, 0);
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

/* The whole menu again (a page change resizes the box) */
static void menu_redraw(void) {
	fill(0, 0, BUF_W, BUF_H, 0);
	draw_menu();
	present_synced();
}

static void perf_save(void);

/* B, or a page's last row: settings to the system page, that to the app
   page (over an app), else out of the menu */
static void menu_back(void) {
	sRepeatDir = 0;
	if (sMenuPage == 4) {
		sMenuPage = 3;
		sMenuSel = SET_USB;
		menu_redraw();
	} else if (sMenuPage == 3) {
		sMenuPage = 1;
		sMenuSel = SYS_SETTINGS;
		menu_redraw();
	} else if (sMenuPage == 1 && !sMenuLauncher) {
		sMenuPage = 0;
		sMenuSel = APP_SYSTEM;
		menu_redraw();
	} else {
		menu_close(0);
	}
}

static void menu_confirm(void) {
	if (!sMenuPage) {
		if (sMenuSel == APP_SYSTEM) {
			sMenuPage = 1;
			sMenuSel = SYS_SETTINGS;
			menu_redraw();
		} else if (sMenuSel == APP_SCREENSHOT) {
			take_screenshot();
			menu_close(0);
		} else {
			menu_close(sMenuSel == APP_EXIT);
		}
		return;
	}
	if (sMenuPage == 4) {
		// Reboot into the loader's USB disk mode (as holding SELECT at power
		// on): the card can't be exported live, the root squashfs is
		// loop-mounted from it
		if (sMenuSel == USB_OK && usb_mode_flag() == 0) {
			menu_close(0);
			run_system("reboot");
		} else {
			menu_back();
		}
		return;
	}
	if (sMenuPage == 1) {
		switch (sMenuSel) {
			case SYS_SETTINGS:
				sMenuPage = 3;
				sMenuSel = SET_BRIGHTNESS;
				menu_redraw();
				break;
			case SYS_SLEEP:
				system_sleep();
				menu_close(0);
				break;
			case SYS_REBOOT:
			case SYS_POWEROFF:
				menu_close(0); // the app runs again, so init's SIGTERM reaches it
				run_system(sMenuSel == SYS_REBOOT ? "reboot" : "poweroff");
				break;
			case SYS_BACK:
				menu_back();
				break;
		}
		return;
	}
	switch (sMenuSel) {
		case SET_PERF:
			sPerf = (sPerf + 1) % PERF_MODES;
			perf_save();
			menu_redraw();
			break;
		case SET_USB_VIDEO:
			usb_video_toggle();
			menu_redraw();
			break;
		case SET_CALIBRATE:
			cal_open(); // drawn by cal_frame
			break;
		case SET_USB:
			sMenuPage = 4; // confirm first: it reboots
			sMenuSel = USB_CANCEL;
			menu_redraw();
			break;
		case SET_BACK:
			menu_back();
			break;
	} // the brightness slider: left/right only
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
		if (sMenuPage == 2) {
			cal_input(&ev[i]);
			continue;
		}
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
				if (sMenuPage == 4 && sMenuSel < USB_TEXT_ROWS) {
					sMenuSel = ev[i].code == BTN_DPAD_UP ? USB_CANCEL : USB_OK; // over the text
				}
				sRepeatDir = 0;
				redraw = 1;
				break;
			case BTN_DPAD_LEFT:
			case BTN_DPAD_RIGHT:
				if (on_brightness_row() && sBacklight[0] != '\0') {
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
				menu_back();
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
		sAppLauncher = 0;
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
		if (capacity >= 0) {
			osd_show(online ? "charging" : "battery", capacity);
		}
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
			sAppLauncher = is_launcher(sAppPid);
		}
		reply(c, fd >= 0 ? "ok" : "fail", fd);
		if (fd >= 0) {
			close(fd); // the client has its own copy now
		}
	} else if (sscanf(msg, "osd %15s %d", icon, &percent) == 2) {
		osd_show(icon, percent);
	}
}

/* ---------------------------------------------------------------------------
 * Performance overlay (System > Overlay), like MangoHud: FPS and frame
 * times of the app on screen (from the page flips libs30lease.so marks in the
 * S30_FRAMES_PATH ring), CPU, GPU, temperatures, memory, battery. A box in
 * the top left corner; the plane covers the whole screen while it is on. Or
 * just the FPS, in a small box there (inside the OSD's strip). The OSD box
 * overlaps it: while the OSD shows, osd_frame paints the overlay under it.
 * ------------------------------------------------------------------------- */

#define PERF_MS 100         // redraw (FPS and the graph)
#define PERF_SAMPLE_MS 1000 // system stats
#define PERF_X 4
#define PERF_W 192 // 22 characters (the TEMP line) and margins
#define PERF_GRAPH_H 28
#define PERF_LINES 7
#define PERF_H (6 + 16 + PERF_GRAPH_H + 4 + (PERF_LINES - 1) * 16 + 6)
#define PERF_Y 4
#define PERF_GRAPH_MS 100.0f // graph full height
#define PERF_FPS_X 4         // the FPS-only box
#define PERF_FPS_Y 4
#define PERF_FPS_W (6 + 6 * 8 + 6) // "FPS 60"
#define PERF_FPS_H (6 + 16 + 6)

static S30Frames* sFrames;
static char sGpuFreq[300], sPmicTemp[300];
static char sPerfLine[PERF_LINES - 1][80]; // system stat lines, label first (with PERF_* colours)
static int sPerfLabelLen[PERF_LINES - 1];
static uint64_t sPerfSampleAt;

static int read_line(const char* path, char* buf, size_t len) {
	FILE* f = fopen(path, "r");
	int ok = f != NULL && fgets(buf, len, f) != NULL;

	if (f != NULL) {
		fclose(f);
	}
	return ok;
}

static long read_long(const char* path) {
	char buf[64];

	return read_line(path, buf, sizeof(buf)) ? atol(buf) : -1;
}

static void perf_save(void) {
	FILE* f;

	if (sPerfFile[0] == '\0') {
		return;
	}
	f = fopen(sPerfFile, "w");
	if (f != NULL) {
		fprintf(f, "%d\n", sPerf);
		fclose(f);
	}
}

/* Where things are; the saved switch (next to the brightness file) */
static void perf_init(void) {
	DIR* d;
	struct dirent* e;
	int fd;

	if (sBrightFile != NULL) {
		const char* slash = strrchr(sBrightFile, '/');
		int dirLen = slash != NULL ? (int)(slash - sBrightFile) : 0;

		snprintf(sPerfFile, sizeof(sPerfFile), "%.*s%sperf_overlay", dirLen, sBrightFile, dirLen ? "/" : "");
		sPerf = read_int(sPerfFile);
		sPerf = sPerf > 0 && sPerf < PERF_MODES ? sPerf : PERF_OFF;
	}
	d = opendir("/sys/class/devfreq");
	while (d != NULL && (e = readdir(d)) != NULL) {
		if (strstr(e->d_name, "gpu") != NULL) {
			snprintf(sGpuFreq, sizeof(sGpuFreq), "/sys/class/devfreq/%s/cur_freq", e->d_name);
		}
	}
	if (d != NULL) {
		closedir(d);
	}
	// The PMIC's IIO device (the SoC sensor is a thermal zone)
	d = opendir("/sys/bus/iio/devices");
	while (d != NULL && (e = readdir(d)) != NULL) {
		char link[300], target[300];
		ssize_t n;

		snprintf(link, sizeof(link), "/sys/bus/iio/devices/%s", e->d_name);
		n = readlink(link, target, sizeof(target) - 1);
		if (n > 0 && (target[n] = '\0', strstr(target, "axp") != NULL)) {
			snprintf(sPmicTemp, sizeof(sPmicTemp), "%s/in_temp", link);
		}
	}
	if (d != NULL) {
		closedir(d);
	}
	// The frame ring the apps' shim writes
	unlink(S30_FRAMES_PATH);
	fd = open(S30_FRAMES_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
	if (fd >= 0) {
		fchmod(fd, 0666);
		if (ftruncate(fd, sizeof(S30Frames)) == 0) {
			void* m = mmap(NULL, sizeof(S30Frames), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			sFrames = m == MAP_FAILED ? NULL : m;
		}
		close(fd);
	}
}

/* CPU load per core and in total since the last call (/proc/stat) */
static void cpu_load(int* total, int core[4]) {
	static unsigned long long prevBusy[5], prevAll[5];
	char line[256];
	FILE* f = fopen("/proc/stat", "r");
	int i;

	*total = 0;
	for (i = 0; i < 4; i++) {
		core[i] = -1;
	}
	while (f != NULL && fgets(line, sizeof(line), f) != NULL && strncmp(line, "cpu", 3) == 0) {
		unsigned long long v[8] = { 0 }, all = 0, busy;
		int idx = line[3] == ' ' ? 0 : atoi(line + 3) + 1, k;
		char* p = line + 3;

		if (idx > 4) {
			continue;
		}
		while (*p != ' ' && *p != '\0') {
			p++;
		}
		sscanf(p, "%llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
		for (k = 0; k < 8; k++) {
			all += v[k];
		}
		busy = all - v[3] - v[4]; // minus idle and iowait
		if (all > prevAll[idx]) {
			int pct = (int)(100 * (busy - prevBusy[idx]) / (all - prevAll[idx]));
			if (idx == 0) {
				*total = pct;
			} else {
				core[idx - 1] = pct;
			}
		}
		prevBusy[idx] = busy;
		prevAll[idx] = all;
	}
	if (f != NULL) {
		fclose(f);
	}
}

static long meminfo(const char* key) {
	char line[128];
	FILE* f = fopen("/proc/meminfo", "r");
	long v = -1;

	while (f != NULL && fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, key, strlen(key)) == 0) {
			v = atol(line + strlen(key));
			break;
		}
	}
	if (f != NULL) {
		fclose(f);
	}
	return v;
}

/* Colour switches inside the stat lines (perf_text) */
#define PERF_WHITE "\x01"
#define PERF_GREEN "\x02"
#define PERF_YELLOW "\x03"
#define PERF_RED "\x04"
#define PERF_CYAN "\x05"
static const uint32_t kPerfColours[] = { 0, 0xffffff, 0x60d060, 0xe0c040, 0xe04040, 0x60c0e0 };

/* Green below `warn`, yellow below `hot`, red from there */
static const char* perf_level(double v, double warn, double hot) {
	return v < warn ? PERF_GREEN : v < hot ? PERF_YELLOW : PERF_RED;
}

/* draw_text with the PERF_* colour switches, starting in `rgb` */
static void perf_text(const char* text, int x, int y, uint32_t rgb) {
	char one[2] = { 0, 0 };

	for (; *text != '\0'; text++) {
		if ((unsigned char)*text < sizeof(kPerfColours) / sizeof(kPerfColours[0])) {
			rgb = kPerfColours[(unsigned char)*text];
			continue;
		}
		one[0] = *text;
		draw_text(one, x, y, 1, argb(rgb, 1.0f));
		x += 8;
	}
}

/* The system stat lines, once a second */
static void perf_sample(void) {
	char path[340], buf[64];
	int total, core[4], n = 0, capacity, charging;
	long soc = read_long("/sys/class/thermal/thermal_zone0/temp"), pmic = -1000000, v = -1, ma = 0;
	long memTotal = meminfo("MemTotal:"), memAvail = meminfo("MemAvailable:");
	DIR* d;
	struct dirent* e;

	cpu_load(&total, core);
#define LINE(label, ...)                                                                       \
	do {                                                                                       \
		int l_ = snprintf(sPerfLine[n], sizeof(sPerfLine[n]), "%s", label);                    \
		snprintf(sPerfLine[n] + l_, sizeof(sPerfLine[n]) - l_, __VA_ARGS__);                   \
		sPerfLabelLen[n++] = l_;                                                               \
	} while (0)
	LINE("CPU ", "%s%3d%% " PERF_CYAN "%4ldMHz", perf_level(total, 50, 80), total,
	     read_long("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq") / 1000);
	LINE("    ", "%s%3d %s%3d %s%3d %s%3d", perf_level(core[0], 50, 80), core[0], perf_level(core[1], 50, 80), core[1],
	     perf_level(core[2], 50, 80), core[2], perf_level(core[3], 50, 80), core[3]);
	LINE("GPU ", PERF_CYAN "%4ldMHz", sGpuFreq[0] ? read_long(sGpuFreq) / 1000000 : 0);
	if (sPmicTemp[0]) {
		long raw, ofs, scale;
		snprintf(path, sizeof(path), "%s_raw", sPmicTemp);
		raw = read_long(path);
		snprintf(path, sizeof(path), "%s_offset", sPmicTemp);
		ofs = read_long(path);
		snprintf(path, sizeof(path), "%s_scale", sPmicTemp);
		scale = read_long(path);
		pmic = (raw + ofs) * scale; // millidegrees
	}
	if (soc > -1000000 && soc != -1) {
		LINE("TEMP", " SoC %s%2ld" OSD_DEGREE "C" PERF_WHITE " PMU %s%2ld" OSD_DEGREE "C", perf_level(soc / 1000, 60, 75),
		     soc / 1000, perf_level(pmic / 1000, 60, 75), pmic / 1000);
	} else {
		LINE("TEMP", " PMU %s%2ld" OSD_DEGREE "C", perf_level(pmic / 1000, 60, 75), pmic / 1000);
	}
	LINE("RAM ", "%s%3ld" PERF_WHITE "/%3ldMB", perf_level(memTotal - memAvail, memTotal * 0.7, memTotal * 0.9),
	     (memTotal - memAvail) / 1024, memTotal / 1024);
	external_power(&capacity, &charging);
	d = opendir(POWER_SUPPLY);
	while (d != NULL && (e = readdir(d)) != NULL) {
		if (e->d_name[0] != '.' && read_sysfs(e->d_name, "type", buf, sizeof(buf)) && strcmp(buf, "Battery") == 0) {
			snprintf(path, sizeof(path), POWER_SUPPLY "/%s/voltage_now", e->d_name);
			v = read_long(path);
			snprintf(path, sizeof(path), POWER_SUPPLY "/%s/current_now", e->d_name);
			ma = read_long(path) / 1000;
			if (!charging && ma > 0) {
				ma = -ma; // drawn from the battery
			}
		}
	}
	if (d != NULL) {
		closedir(d);
	}
	LINE("BAT ", "%s%3d%%" PERF_WHITE " %ld.%02ldV %s%+ldmA", capacity > 50 ? PERF_GREEN : capacity > 20 ? PERF_YELLOW : PERF_RED,
	     capacity, v / 1000000, v % 1000000 / 10000, ma > 0 ? PERF_GREEN : PERF_WHITE, ma);
#undef LINE
}

/* Age of a frame stamped at t: 0 if it is newer than now (nowNs comes from a
   millisecond clock, so a frame marked in the same millisecond is ahead of
   it; unsigned, the difference would wrap and the frame look ancient) */
static uint64_t frame_age(uint64_t t, uint64_t nowNs) {
	return t > nowNs ? 0 : nowNs - t;
}

/* Frame intervals (ms) of the app on screen, newest last, into out[max];
   count returned; *fps over the last second */
static int frame_times(float* out, int max, int* fps, uint64_t nowNs) {
	uint32_t count, i, first;
	int n = 0;

	*fps = 0;
	if (sFrames == NULL || (count = sFrames->count) < 2) {
		return 0;
	}
	__sync_synchronize();
	if (frame_age(sFrames->ns[(count - 1) % S30_FRAMES_N], nowNs) > 1000000000ull) {
		return 0; // no frames for a second: nothing to show
	}
	first = count > (uint32_t)max + 1 ? count - (uint32_t)max - 1 : 0;
	first = count - first > S30_FRAMES_N ? count - S30_FRAMES_N : first;
	for (i = first + 1; i < count; i++) {
		uint64_t a = sFrames->ns[(i - 1) % S30_FRAMES_N], b = sFrames->ns[i % S30_FRAMES_N];
		out[n++] = (b - a) / 1e6f;
	}
	for (i = count; i > 0 && i + S30_FRAMES_N > count; i--) {
		if (frame_age(sFrames->ns[(i - 1) % S30_FRAMES_N], nowNs) > 1000000000ull) {
			break;
		}
		(*fps)++;
	}
	return n;
}

static void perf_paint_fps(uint64_t nowMs) {
	const int x0 = PERF_FPS_X + 6, y0 = PERF_FPS_Y + 6;
	float ft[1];
	int fps;
	char text[8];

	if (frame_times(ft, 1, &fps, nowMs * 1000000ull)) {
		snprintf(text, sizeof(text), "%3d", fps);
	} else {
		snprintf(text, sizeof(text), "  -");
	}
	fill(PERF_FPS_X, PERF_FPS_Y, PERF_FPS_W, PERF_FPS_H, 0);
	draw_box(PERF_FPS_X, PERF_FPS_Y, PERF_FPS_W, PERF_FPS_H, 0.6f);
	draw_text("FPS", x0, y0, 1, argb(0xe0a050, 1.0f));
	draw_text(text, x0 + 3 * 8, y0, 1, argb(0x60c0e0, 1.0f));
}

static void perf_paint_full(uint64_t nowMs) {
	const int x0 = PERF_X + 6, gy = PERF_Y + 6 + 16 + 2, bars = (PERF_W - 12) / 2;
	float ft[96];
	int n, fps, i, y;
	char text[40];
	float avg = 0, worst = 0;

	n = frame_times(ft, bars, &fps, nowMs * 1000000ull);
	for (i = 0; i < n; i++) {
		avg += ft[i];
		worst = ft[i] > worst ? ft[i] : worst;
	}
	avg = n ? avg / n : 0;
	fill(PERF_X, PERF_Y, PERF_W, PERF_H, 0);
	draw_box(PERF_X, PERF_Y, PERF_W, PERF_H, 0.6f);
	// FPS and frame time
	draw_text("FPS", x0, PERF_Y + 6, 1, argb(0xe0a050, 1.0f));
	if (n) {
		snprintf(text, sizeof(text), "%3d %5.1fms", fps, avg);
	} else {
		snprintf(text, sizeof(text), "  - ");
	}
	draw_text(text, x0 + 4 * 8, PERF_Y + 6, 1, argb(0x60c0e0, 1.0f));
	// Frame time graph, newest at the right: bars to PERF_GRAPH_MS; a spike
	// (over 1.5x the average) in red; a line at the average
	fill(x0, gy, PERF_W - 12, PERF_GRAPH_H, argb(0x000000, 0.35f));
	for (i = 0; i < n; i++) {
		int h = (int)(ft[i] / PERF_GRAPH_MS * PERF_GRAPH_H + 0.5f);
		int bx = x0 + (bars - n + i) * 2;

		h = h < 1 ? 1 : h > PERF_GRAPH_H ? PERF_GRAPH_H : h;
		fill(bx, gy + PERF_GRAPH_H - h, 2, h, argb(ft[i] > avg * 1.5f ? 0xe04040 : 0x60d060, 1.0f));
	}
	if (n) {
		int ay = gy + PERF_GRAPH_H - (int)(avg / PERF_GRAPH_MS * PERF_GRAPH_H + 0.5f);
		if (ay >= gy) {
			fill(x0, ay, PERF_W - 12, 1, argb(0xffffff, 0.5f));
		}
	}
	(void)worst;
	// System stats
	y = gy + PERF_GRAPH_H + 4;
	for (i = 0; i < PERF_LINES - 1; i++, y += 16) {
		char label[8];

		snprintf(label, sizeof(label), "%.*s", sPerfLabelLen[i], sPerfLine[i]);
		draw_text(label, x0, y, 1, argb(0xe0a050, 1.0f));
		perf_text(sPerfLine[i] + sPerfLabelLen[i], x0 + sPerfLabelLen[i] * 8, y, 0xffffff);
	}
}

/* Whether the overlay is on screen now */
static int perf_visible(void) {
	return sPerf && !sAppLauncher && !sMenu && sMaster && sLessee != 0;
}

/* Draws the overlay into the buffer (not presented) */
static void perf_paint(uint64_t nowMs) {
	if (sPerf == PERF_FPS) {
		perf_paint_fps(nowMs);
	} else {
		perf_paint_full(nowMs);
	}
}

/* Main loop step: the overlay over an app, every PERF_MS. Returns the time
   to the next step (ms), or -1 when off. Not over the launcher: it only
   draws when something changes, so its FPS says nothing. */
static int perf_frame(uint64_t t) {
	if (!perf_visible()) {
		return -1;
	}
	if (!sPlaneOn || sShowH != plane_h()) {
		// The plane over the whole screen, or the top strip for the FPS alone
		// (set once per app, like the OSD's)
		fill(0, 0, BUF_W, BUF_H, 0);
		present_rect(0, 0, BUF_W, BUF_H);
		set_plane(0, 0, BUF_W, plane_h());
		sPerfNext = 0;
	}
	if (t < sPerfNext) {
		return (int)(sPerfNext - t);
	}
	if (sPerf == PERF_FULL && t >= sPerfSampleAt) {
		perf_sample();
		sPerfSampleAt = t + PERF_SAMPLE_MS;
	}
	if (sAlpha > 0.0f || t < sOsdUntil) {
		sDirty = 1; // the OSD over it: osd_frame draws both
	} else {
		perf_paint(t);
		wait_vblank();
		if (sPerf == PERF_FPS) {
			present_rect(PERF_FPS_X, PERF_FPS_Y, PERF_FPS_W, PERF_FPS_H);
		} else {
			present_rect(PERF_X, PERF_Y, PERF_W, PERF_H);
		}
	}
	sPerfNext = t + PERF_MS;
	return PERF_MS;
}

int main(int argc, char** argv) {
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int srv, nl, power, i, timeout, perfWait;

	setvbuf(stdout, NULL, _IOLBF, 0);
	sBrightFile = argc > 1 ? argv[1] : NULL;
	brightness_init();
	perf_init();
	{
		// Wake promptly after a vblank (short bursts; blocks otherwise).
		// musl's sched_setscheduler() is a stub: the system call itself.
		// Children (s30-screenshot, usb-video and the s30-uvc it starts)
		// get the normal policy back.
		struct sched_param sp = { .sched_priority = 1 };

		syscall(SYS_sched_setscheduler, 0, SCHED_FIFO | SCHED_RESET_ON_FORK, &sp);
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
		animating |= cal_frame();
		perfWait = perf_frame(now_ms());

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
		if (perfWait >= 0 && perfWait < timeout) {
			timeout = perfWait; // next overlay update
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
