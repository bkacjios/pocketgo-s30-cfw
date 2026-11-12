/*
 * The screen as shown, read back from the display's KMS planes (shared by
 * s30-screenshot and s30-uvc).
 *
 * Every plane on the panel's CRTC with a framebuffer is read through GETFB2
 * (root: the buffer handles need CAP_SYS_ADMIN), mapped as a dumb buffer and
 * blended in zpos order, the alpha formats over what is below. Nothing has
 * to cooperate: it works over the launcher, a game or s30-display's menu.
 * The buffers are write-combined (uncached), so each visible row is copied
 * out in one burst before its pixels are read.
 */
#ifndef S30_CAPTURE_H
#define S30_CAPTURE_H

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

/* The panel is 320x480 portrait, held landscape with its +x pointing down:
   landscape (x, y) is panel (y, 479 - x) */
#define PANEL_W 320
#define PANEL_H 480

typedef struct {
	int fd;        // the KMS card
	uint32_t crtc; // the panel's CRTC
} S30Capture;

static uint64_t s30_capture_prop(int fd, uint32_t plane, const char* name, uint64_t def) {
	drmModeObjectProperties* props = drmModeObjectGetProperties(fd, plane, DRM_MODE_OBJECT_PLANE);
	uint64_t v = def;
	uint32_t i;

	for (i = 0; props != NULL && i < props->count_props; i++) {
		drmModePropertyRes* p = drmModeGetProperty(fd, props->props[i]);

		if (p != NULL && strcmp(p->name, name) == 0) {
			v = props->prop_values[i];
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return v;
}

/* The display card (the GPU has its own) and its active CRTC; -1 if none */
static int s30_capture_open(S30Capture* c) {
	drmModeRes* res = NULL;
	int i;

	c->fd = -1;
	c->crtc = 0;
	for (i = 0; i < 4 && res == NULL; i++) {
		char path[32];

		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		c->fd = open(path, O_RDWR | O_CLOEXEC);
		if (c->fd >= 0 && (res = drmModeGetResources(c->fd)) != NULL && res->count_crtcs > 0) {
			break;
		}
		drmModeFreeResources(res);
		res = NULL;
		if (c->fd >= 0) {
			close(c->fd);
			c->fd = -1;
		}
	}
	if (res == NULL) {
		fprintf(stderr, "s30-capture: no display card\n");
		return -1;
	}
	for (i = 0; i < res->count_crtcs && c->crtc == 0; i++) {
		drmModeCrtc* crtc = drmModeGetCrtc(c->fd, res->crtcs[i]);

		if (crtc != NULL && crtc->mode_valid) {
			c->crtc = crtc->crtc_id;
		}
		drmModeFreeCrtc(crtc);
	}
	drmModeFreeResources(res);
	// The first to open the card becomes its master, which would keep
	// s30-display (the lessor) from becoming master; reading the planes
	// needs only root
	drmDropMaster(c->fd);
	drmSetClientCap(c->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	return 0;
}

/* One plane's pixels blended into `screen` (panel layout, 0xRRGGBB) */
static int s30_capture_plane(int fd, uint32_t plane, uint32_t* screen) {
	static uint8_t line[4096 * 4];
	drmModePlane* pl = drmModeGetPlane(fd, plane);
	drmModeFB2* fb;
	struct drm_mode_map_dumb map = { 0 };
	struct drm_gem_close gemClose = { 0 };
	int cx, cy, cw, ch, sx, sy, x, y, alpha, bpp, rowPx;
	uint8_t* mem;
	size_t size;

	if (pl == NULL || pl->fb_id == 0) {
		drmModeFreePlane(pl);
		return 0;
	}
	fb = drmModeGetFB2(fd, pl->fb_id);
	drmModeFreePlane(pl);
	if (fb == NULL || fb->handles[0] == 0) {
		fprintf(stderr, "s30-capture: plane %u: no buffer handle (not root?)\n", plane);
		drmModeFreeFB2(fb);
		return -1;
	}
	gemClose.handle = fb->handles[0]; // GETFB2 opened it for us
	switch (fb->pixel_format) {
		case DRM_FORMAT_XRGB8888:
		case DRM_FORMAT_ARGB8888:
			bpp = 4;
			break;
		case DRM_FORMAT_RGB565:
			bpp = 2;
			break;
		default:
			fprintf(stderr, "s30-capture: plane %u: format %.4s not handled\n", plane,
			        (const char*)&fb->pixel_format);
			drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &gemClose);
			drmModeFreeFB2(fb);
			return -1;
	}
	alpha = fb->pixel_format == DRM_FORMAT_ARGB8888;
	map.handle = fb->handles[0];
	size = (size_t)fb->pitches[0] * fb->height + fb->offsets[0];
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0 ||
	    (mem = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, map.offset)) == MAP_FAILED) {
		perror("s30-capture: map");
		drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &gemClose);
		drmModeFreeFB2(fb);
		return -1;
	}
	// Where it shows (CRTC_*) and what part of the buffer (SRC_*, 16.16)
	cx = (int)(int64_t)s30_capture_prop(fd, plane, "CRTC_X", 0);
	cy = (int)(int64_t)s30_capture_prop(fd, plane, "CRTC_Y", 0);
	cw = (int)s30_capture_prop(fd, plane, "CRTC_W", fb->width);
	ch = (int)s30_capture_prop(fd, plane, "CRTC_H", fb->height);
	sx = (int)(s30_capture_prop(fd, plane, "SRC_X", 0) >> 16);
	sy = (int)(s30_capture_prop(fd, plane, "SRC_Y", 0) >> 16);
	rowPx = cw;
	if (sx + rowPx > (int)fb->width) {
		rowPx = (int)fb->width - sx;
	}
	if (rowPx * bpp > (int)sizeof(line)) {
		rowPx = (int)sizeof(line) / bpp;
	}
	for (y = 0; y < ch && sy + y < (int)fb->height; y++) {
		if (cy + y < 0 || cy + y >= PANEL_H) {
			continue;
		}
		memcpy(line, mem + fb->offsets[0] + (size_t)(sy + y) * fb->pitches[0] + (size_t)sx * bpp,
		       (size_t)rowPx * bpp);
		for (x = 0; x < rowPx; x++) {
			const uint8_t* s = line + (size_t)x * bpp;
			uint32_t* d;
			uint32_t c, a;

			if (cx + x < 0 || cx + x >= PANEL_W) {
				continue;
			}
			d = &screen[(cy + y) * PANEL_W + cx + x];
			if (bpp == 2) {
				uint16_t v = (uint16_t)(s[0] | s[1] << 8);
				*d = ((v >> 11) * 255 / 31) << 16 | (((v >> 5) & 63) * 255 / 63) << 8 | (v & 31) * 255 / 31;
				continue;
			}
			c = (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16;
			a = alpha ? s[3] : 255;
			if (a == 255) {
				*d = c;
			} else if (a != 0) { // straight alpha over what is below
				uint32_t r = (((c >> 16) & 255) * a + ((*d >> 16) & 255) * (255 - a)) / 255;
				uint32_t g = (((c >> 8) & 255) * a + ((*d >> 8) & 255) * (255 - a)) / 255;
				uint32_t b = ((c & 255) * a + (*d & 255) * (255 - a)) / 255;
				*d = r << 16 | g << 8 | b;
			}
		}
	}
	munmap(mem, size);
	drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &gemClose);
	drmModeFreeFB2(fb);
	return 0;
}

/* The screen into `screen` (panel layout, PANEL_W x PANEL_H, 0xRRGGBB).
   gameOnly: the primary plane only (no OSD, menu or performance overlay). */
static int s30_capture(const S30Capture* c, int gameOnly, uint32_t* screen) {
	drmModePlaneRes* planes = drmModeGetPlaneResources(c->fd);
	uint32_t order[16];
	uint64_t zpos[16];
	int i, j, n = 0;

	memset(screen, 0, sizeof(uint32_t) * PANEL_W * PANEL_H);
	for (i = 0; planes != NULL && i < (int)planes->count_planes && n < 16; i++) {
		drmModePlane* p = drmModeGetPlane(c->fd, planes->planes[i]);

		if (p != NULL && p->crtc_id == c->crtc && p->fb_id != 0 &&
		    (!gameOnly ||
		     s30_capture_prop(c->fd, p->plane_id, "type", DRM_PLANE_TYPE_OVERLAY) == DRM_PLANE_TYPE_PRIMARY)) {
			// Sorted by zpos as found
			uint64_t z = s30_capture_prop(c->fd, p->plane_id, "zpos", (uint64_t)i);
			for (j = n; j > 0 && zpos[j - 1] > z; j--) {
				zpos[j] = zpos[j - 1];
				order[j] = order[j - 1];
			}
			zpos[j] = z;
			order[j] = p->plane_id;
			n++;
		}
		drmModeFreePlane(p);
	}
	drmModeFreePlaneResources(planes);
	for (i = 0; i < n; i++) {
		s30_capture_plane(c->fd, order[i], screen);
	}
	return 0;
}

#endif
