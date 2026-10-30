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
 * Socket messages (SOCK_SEQPACKET, /run/s30-display.sock):
 *   "lease PATH"             PATH being opened; for the display card reply "ok"
 *                            with the lease fd attached, else "fail"
 *   "osd ICON PERCENT"       ICON: speaker, headphones, mute, charging,
 *                            battery; PERCENT 0-100
 *
 * Usage: s30-display
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <dirent.h>
#include <linux/netlink.h>
#include <time.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define SOCKET_PATH "/run/s30-display.sock"
#define OSD_MS 1500       // OSD visible after the last message
#define FADE_STEP 0.17f   // alpha per frame: about 100 ms fades
#define EASE 0.3f         // fraction of the remaining bar distance per frame
#define LEASE_POLL_MS 250 // check whether the lessee has gone

/* The panel is 320x480 portrait, held landscape with its +x pointing down:
   landscape (x right, y down; 480x320) to panel is px = y, py = 479 - x */
#define PANEL_W 320
#define PANEL_H 480

/* OSD box in landscape coordinates: centred near the top */
#define BOX_W 220
#define BOX_H 40
#define BOX_X ((PANEL_H - BOX_W) / 2)
#define BOX_Y 12

static int sFd = -1;
static char sCard[32]; // the display (KMS) card
static uint32_t sConnector, sCrtc, sPrimary, sOverlay;
static uint32_t sLessee;     // 0: no lease
static int sMaster;
static uint32_t sFb;         // OSD buffer, panel orientation (BOX_H x BOX_W)
static uint32_t* sPixels;    // the plane's buffer (scanned out)
static uint32_t sPitch;      // in pixels
static uint32_t sBack[BOX_W * BOX_H]; // frame being drawn, panel layout, pitch BOX_H
static int sPlaneOn;          // overlay plane enabled (for the current lease)
static char sIcon[16] = "speaker";
static float sTarget, sShown; // bar percent: requested, drawn
static float sAlpha;          // box opacity drawn
static int sDirty;            // needs a redraw
static uint64_t sOsdUntil, sNextLeaseCheck;

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
	struct drm_mode_create_dumb create = { .width = BOX_H, .height = BOX_W, .bpp = 32 };
	struct drm_mode_map_dumb map = { 0 };
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };

	if (drmIoctl(sFd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
		return -1;
	}
	sPitch = create.pitch / 4;
	handles[0] = create.handle;
	pitches[0] = create.pitch;
	if (drmModeAddFB2(sFd, BOX_H, BOX_W, DRM_FORMAT_ARGB8888, handles, pitches, offsets, &sFb, 0) != 0) {
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
 * Drawing, in box-local landscape coordinates (BOX_W x BOX_H)
 * ------------------------------------------------------------------------- */

static void put(int x, int y, uint32_t argb) {
	if (x >= 0 && x < BOX_W && y >= 0 && y < BOX_H) {
		// landscape (x, y) -> panel (y, 479 - x), relative to the box
		sBack[(BOX_W - 1 - x) * BOX_H + y] = argb;
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

/* 16x16 icons, drawn at 2x */
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

static void draw_icon(const char* const* icon, int x0, int y0, uint32_t argb) {
	int x, y;

	for (y = 0; y < 16; y++) {
		for (x = 0; x < 16; x++) {
			if (icon[y][x] == '#') {
				fill(x0 + x * 2, y0 + y * 2, 2, 2, argb);
			}
		}
	}
}

/* The box at opacity `alpha`, bar at `percent` (fractional: the bar's end
   pixel is blended, so it moves smoothly) */
static void draw_osd(const char* icon, float percent, float alpha) {
	const int barX = 52, barW = BOX_W - barX - 14, barY = (BOX_H - 8) / 2;
	const uint32_t track = 0x404048, bar = 0x8060e0;
	int corner[4] = { 3, 2, 1, 1 }, i, full;
	float end, frac;

	if (alpha <= 0.0f) {
		fill(0, 0, BOX_W, BOX_H, 0);
		return;
	}
	percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	fill(0, 0, BOX_W, BOX_H, argb(0x101018, 0.75f * alpha));
	for (i = 0; i < 4; i++) { // rounded corners
		fill(0, i, corner[i], 1, 0);
		fill(BOX_W - corner[i], i, corner[i], 1, 0);
		fill(0, BOX_H - 1 - i, corner[i], 1, 0);
		fill(BOX_W - corner[i], BOX_H - 1 - i, corner[i], 1, 0);
	}
	draw_icon(strcmp(icon, "headphones") == 0 ? kHeadphones
	          : strcmp(icon, "mute") == 0     ? kMute
	          : strcmp(icon, "charging") == 0 ? kCharging
	          : strcmp(icon, "battery") == 0  ? kBattery
	                                          : kSpeaker,
	          10, (BOX_H - 32) / 2, argb(0xffffff, alpha));
	end = barW * percent / 100.0f;
	full = (int)end;
	frac = end - full;
	fill(barX, barY, barW, 8, argb(track, alpha));
	fill(barX, barY, full, 8, argb(bar, alpha));
	if (full < barW && frac > 0.0f) {
		// end pixel: bar over track by coverage
		uint32_t r = (uint32_t)(((bar >> 16) & 0xff) * frac + ((track >> 16) & 0xff) * (1 - frac));
		uint32_t g = (uint32_t)(((bar >> 8) & 0xff) * frac + ((track >> 8) & 0xff) * (1 - frac));
		uint32_t b = (uint32_t)((bar & 0xff) * frac + (track & 0xff) * (1 - frac));
		fill(barX + full, barY, 1, 8, argb((r << 16) | (g << 8) | b, alpha));
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

/* Back buffer to the scanned-out one */
static void present(void) {
	int row;

	for (row = 0; row < BOX_W; row++) {
		memcpy(&sPixels[row * sPitch], &sBack[row * BOX_H], BOX_H * sizeof(uint32_t));
	}
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
		return; // nobody on screen through a lease: the display isn't ours
	}
	snprintf(sIcon, sizeof(sIcon), "%s", icon);
	sTarget = percent;
	if (sAlpha <= 0.0f) {
		sShown = percent; // appear at the value; slide only while visible
	}
	sOsdUntil = now_ms() + OSD_MS;
	sDirty = 1;
	if (!sPlaneOn) {
		draw_osd(sIcon, sShown, 0.0f); // starts transparent, fades in
		present();
		// Panel rectangle of the landscape box: x = BOX_Y, y = 479 - (BOX_X + BOX_W - 1)
		if (drmModeSetPlane(sFd, sOverlay, sCrtc, sFb, 0, BOX_Y, PANEL_H - BOX_X - BOX_W, BOX_H, BOX_W, 0, 0,
		                    BOX_H << 16, BOX_W << 16) == 0) {
			sPlaneOn = 1;
		} else {
			fprintf(stderr, "s30-display: overlay: %s\n", strerror(errno));
		}
	}
}

/* One frame of fading/easing, drawn just after a vblank (no tearing).
   Returns whether more frames are needed. */
static int osd_frame(void) {
	float want = now_ms() < sOsdUntil ? 1.0f : 0.0f;
	drmVBlank vbl = { .request = { .type = DRM_VBLANK_RELATIVE, .sequence = 1 } };

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
	draw_osd(sIcon, sShown, sAlpha);
	drmWaitVBlank(sFd, &vbl);
	present();
	sDirty = 0;
	return 1;
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
		plane_off();
		sLessee = 0;
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

/* Whether a mains or USB supply is online; battery capacity in *capacity */
static int external_power(int* capacity) {
	DIR* d = opendir(POWER_SUPPLY);
	struct dirent* e;
	char buf[32];
	int online = 0;

	*capacity = -1;
	while (d != NULL && (e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.' || !read_sysfs(e->d_name, "type", buf, sizeof(buf))) {
			continue;
		}
		if (strcmp(buf, "Battery") == 0) {
			if (read_sysfs(e->d_name, "capacity", buf, sizeof(buf))) {
				*capacity = atoi(buf);
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

/* A uevent arrived: show the OSD if external power came or went */
static void power_changed(int nl) {
	char buf[2048];
	ssize_t n = recv(nl, buf, sizeof(buf) - 1, MSG_DONTWAIT);
	int online, capacity, i, power = 0;

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
	online = external_power(&capacity);
	if (online != sPowerOnline) {
		sPowerOnline = online;
		osd_show(online ? "charging" : "battery", capacity < 0 ? 0 : capacity);
	}
}

static int open_uevents(void) {
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = 1 };
	int nl = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
	int capacity;

	if (nl >= 0 && bind(nl, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
		close(nl);
		nl = -1;
	}
	sPowerOnline = external_power(&capacity); // the state at start is not news
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
	char msg[64], icon[16];
	ssize_t n = recv(c, msg, sizeof(msg) - 1, 0);
	int percent;

	if (n <= 0) {
		return;
	}
	msg[n] = '\0';
	if (strncmp(msg, "lease ", 6) == 0) {
		int fd = strcmp(msg + 6, sCard) == 0 ? make_lease() : -1;
		reply(c, fd >= 0 ? "ok" : "fail", fd);
		if (fd >= 0) {
			close(fd); // the client has its own copy now
		}
	} else if (sscanf(msg, "osd %15s %d", icon, &percent) == 2) {
		osd_show(icon, percent);
	}
}

int main(void) {
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int srv, nl, i;

	setvbuf(stdout, NULL, _IOLBF, 0);
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
	printf("s30-display: %s connector %u crtc %u primary %u overlay %u\n", sCard, sConnector, sCrtc, sPrimary,
	       sOverlay);

	for (;;) {
		struct pollfd p[2] = { { srv, POLLIN, 0 }, { nl, POLLIN, 0 } };
		uint64_t t = now_ms();
		int animating = osd_frame(); // waits for a vblank when drawing

		if (t >= sNextLeaseCheck) {
			check_lease();
			sNextLeaseCheck = t + LEASE_POLL_MS;
		}
		if (poll(p, nl >= 0 ? 2 : 1, animating ? 0 : (int)(sOsdUntil > t && sPlaneOn ? sOsdUntil - t : LEASE_POLL_MS)) <=
		    0) {
			continue;
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
