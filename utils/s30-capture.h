/*
 * The screen as shown, read back from the display's KMS planes (shared by
 * s30-screenshot and s30-uvc).
 *
 * Every plane on the panel's CRTC with a framebuffer is read through GETFB2
 * (root: the buffer handles need CAP_SYS_ADMIN), mapped as a dumb buffer and
 * blended in zpos order, the alpha formats over what is below. Nothing has
 * to cooperate: it works over the launcher, a game or s30-display's menu.
 * The buffers are write-combined (uncached, ~105 MB/s to read with memcpy;
 * 64-byte NEON loads with prefetch measured three times slower, 2026-11-17),
 * so the visible rows of every plane are copied out first, back to back,
 * and blended from the copies. The copy must not be preempted when it has
 * to land within a frame (s30-uvc runs SCHED_FIFO for that).
 */
#ifndef S30_CAPTURE_H
#define S30_CAPTURE_H

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

static inline uint64_t s30_capture_prop(int fd, uint32_t plane, const char* name, uint64_t def) {
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
static inline int s30_capture_open(S30Capture* c) {
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

/* One plane, between finding it and blending it */
typedef struct {
	int bpp, alpha;
	int cx, cy, cw, ch; // where it shows (CRTC_*)
	int sx, sy, rowPx;  // the part of the buffer shown (SRC_*), visible row width
	uint32_t pitch, offset, height;
	uint32_t fbId;            // the framebuffer mapped
	struct drm_gem_close gem; // the handle GETFB2 opened for us
	const uint8_t* mem;       // the mapped buffer (write-combined)
	size_t size;
	uint8_t* copy; // the visible rows, copied out (cached)
	size_t copySize;
} S30CapturePlane;

/* A plane's buffer and geometry, mapped; -1 if it can't be read */
/* The plane's current framebuffer (GETFB2) mapped into p; -1 if it has none
   or it can't be mapped. On success p->fbId names it. */
static inline int s30_capture_map(int fd, uint32_t plane, S30CapturePlane* p) {
	drmModePlane* pl = drmModeGetPlane(fd, plane);
	drmModeFB2* fb;
	struct drm_mode_map_dumb map = { 0 };
	void* mem;
	uint32_t fbId;

	p->mem = NULL;
	fbId = pl != NULL ? pl->fb_id : 0;
	drmModeFreePlane(pl);
	if (fbId == 0) {
		return -1;
	}
	fb = drmModeGetFB2(fd, fbId);
	if (fb == NULL || fb->handles[0] == 0) {
		fprintf(stderr, "s30-capture: plane %u: no buffer handle (not root?)\n", plane);
		drmModeFreeFB2(fb);
		return -1;
	}
	memset(&p->gem, 0, sizeof(p->gem));
	p->gem.handle = fb->handles[0];
	switch (fb->pixel_format) {
		case DRM_FORMAT_XRGB8888:
		case DRM_FORMAT_ARGB8888:
			p->bpp = 4;
			break;
		case DRM_FORMAT_RGB565:
			p->bpp = 2;
			break;
		default:
			fprintf(stderr, "s30-capture: plane %u: format %.4s not handled\n", plane,
			        (const char*)&fb->pixel_format);
			drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &p->gem);
			drmModeFreeFB2(fb);
			return -1;
	}
	p->alpha = fb->pixel_format == DRM_FORMAT_ARGB8888;
	p->pitch = fb->pitches[0];
	p->offset = fb->offsets[0];
	p->height = fb->height;
	p->fbId = fbId;
	map.handle = fb->handles[0];
	p->size = (size_t)fb->pitches[0] * fb->height + fb->offsets[0];
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0 ||
	    (mem = mmap(NULL, p->size, PROT_READ, MAP_SHARED, fd, map.offset)) == MAP_FAILED) {
		perror("s30-capture: map");
		drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &p->gem);
		drmModeFreeFB2(fb);
		return -1;
	}
	p->mem = mem;
	drmModeFreeFB2(fb);
	return 0;
}

static inline void s30_capture_unmap(int fd, S30CapturePlane* p) {
	if (p->mem != NULL) {
		munmap((void*)p->mem, p->size);
		p->mem = NULL;
		drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &p->gem);
	}
}

/* The plane's buffer and where it is shown: the geometry (property lookups,
   slow) and the buffer (s30_capture_map) */
static inline int s30_capture_prepare(int fd, uint32_t plane, S30CapturePlane* p) {
	drmModeFB2* fb;

	if (s30_capture_map(fd, plane, p) != 0) {
		return -1;
	}
	fb = drmModeGetFB2(fd, p->fbId);
	if (fb == NULL) {
		s30_capture_unmap(fd, p);
		return -1;
	}
	p->cx = (int)(int64_t)s30_capture_prop(fd, plane, "CRTC_X", 0);
	p->cy = (int)(int64_t)s30_capture_prop(fd, plane, "CRTC_Y", 0);
	p->cw = (int)s30_capture_prop(fd, plane, "CRTC_W", fb->width);
	p->ch = (int)s30_capture_prop(fd, plane, "CRTC_H", fb->height);
	p->sx = (int)(s30_capture_prop(fd, plane, "SRC_X", 0) >> 16);
	p->sy = (int)(s30_capture_prop(fd, plane, "SRC_Y", 0) >> 16);
	p->rowPx = p->cw;
	if (p->sx + p->rowPx > (int)fb->width) {
		p->rowPx = (int)fb->width - p->sx;
	}
	if (p->sy + p->ch > (int)fb->height) {
		p->ch = (int)fb->height - p->sy;
	}
	drmModeFreeFB2(fb);
	return 0;
}

/* The plane's framebuffer now */
static inline uint32_t s30_capture_fb(int fd, uint32_t plane) {
	drmModePlane* pl = drmModeGetPlane(fd, plane);
	uint32_t fbId = pl != NULL ? pl->fb_id : 0;

	drmModeFreePlane(pl);
	return fbId;
}

/* After the vblank: a flip queued before it has applied by now, so the plane
   may show another buffer than prepare mapped; map that one instead (the
   geometry stays). A few ioctls, unlike prepare. */
static inline int s30_capture_rebuffer(int fd, uint32_t plane, S30CapturePlane* p) {
	if (s30_capture_fb(fd, plane) == p->fbId) {
		return 0;
	}
	s30_capture_unmap(fd, p);
	return s30_capture_map(fd, plane, p);
}

/* The visible rows out of the buffer, as fast as it reads: plain memcpy is
   the fastest from write-combined memory here, all rows at once when they
   are contiguous */
static inline int s30_capture_copy(S30CapturePlane* p) {
	size_t row = (size_t)p->rowPx * p->bpp, need = row * (size_t)(p->ch > 0 ? p->ch : 0);
	const uint8_t* src = p->mem + p->offset + (size_t)p->sy * p->pitch + (size_t)p->sx * p->bpp;
	int y;

	if (p->rowPx <= 0 || p->ch <= 0) {
		return -1;
	}
	if (p->copySize < need) {
		uint8_t* c = realloc(p->copy, need);

		if (c == NULL) {
			return -1;
		}
		p->copy = c;
		p->copySize = need;
	}
	if (row == p->pitch) {
		memcpy(p->copy, src, need);
	} else {
		for (y = 0; y < p->ch; y++) {
			memcpy(p->copy + (size_t)y * row, src + (size_t)y * p->pitch, row);
		}
	}
	return 0;
}

/* The copied rows blended into `screen` (panel layout, 0xRRGGBB) */
static inline void s30_capture_blend(const S30CapturePlane* p, uint32_t* screen) {
	const int inside = p->cx >= 0 && p->cx + p->rowPx <= PANEL_W; // no per-pixel bounds checks
	int x, y;

	for (y = 0; y < p->ch; y++) {
		const uint8_t* line = p->copy + (size_t)y * p->rowPx * p->bpp;

		if (p->cy + y < 0 || p->cy + y >= PANEL_H) {
			continue;
		}
		if (inside && p->bpp == 4) {
			// Fast paths: an opaque 32-bit plane is a row copy (the X byte
			// is ignored downstream); an alpha plane skips its transparent
			// pixels on the alpha byte alone
			uint32_t* d = &screen[(p->cy + y) * PANEL_W + p->cx];
			const uint32_t* s32 = (const uint32_t*)line;

			if (!p->alpha) {
				memcpy(d, line, (size_t)p->rowPx * 4);
				continue;
			}
			for (x = 0; x < p->rowPx; x++) {
				uint32_t c = s32[x], a = c >> 24;

				if (a == 255) {
					d[x] = c & 0xffffff;
				} else if (a != 0) {
					uint32_t r = (((c >> 16) & 255) * a + ((d[x] >> 16) & 255) * (255 - a)) / 255;
					uint32_t g = (((c >> 8) & 255) * a + ((d[x] >> 8) & 255) * (255 - a)) / 255;
					uint32_t b = ((c & 255) * a + (d[x] & 255) * (255 - a)) / 255;
					d[x] = r << 16 | g << 8 | b;
				}
			}
			continue;
		}
		for (x = 0; x < p->rowPx; x++) {
			const uint8_t* s = line + (size_t)x * p->bpp;
			uint32_t* d;
			uint32_t c, a;

			if (p->cx + x < 0 || p->cx + x >= PANEL_W) {
				continue;
			}
			d = &screen[(p->cy + y) * PANEL_W + p->cx + x];
			if (p->bpp == 2) {
				uint16_t v = (uint16_t)(s[0] | s[1] << 8);
				*d = ((v >> 11) * 255 / 31) << 16 | (((v >> 5) & 63) * 255 / 63) << 8 | (v & 31) * 255 / 31;
				continue;
			}
			c = (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16;
			a = p->alpha ? s[3] : 255;
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
}

/* The screen into `screen` (panel layout, PANEL_W x PANEL_H, 0xRRGGBB).
   gameOnly: the primary plane only (no OSD, menu or performance overlay).
   vsync: the planes are copied right after the next vblank, the slow
   lookups done before it and the buffers looked up again after it (a flip
   the app queued before the vblank applies at it; the buffer it showed
   before is the one the app draws into next, which is what tore before
   2026-11-17). The buffer on screen after the vblank normally stays there
   for a frame of the app's while the copy, a few ms, runs; when it did
   not (the plane shows another buffer once the copy is done: the app
   flipped again, and may have drawn into the one copied), the copy is
   done again at the next vblank, up to S30_CAPTURE_RETRIES times, so a
   frame is never assembled from a buffer in flux. */
#define S30_CAPTURE_RETRIES 3
#ifdef S30_CAPTURE_TRACE
#include <time.h>
static inline double s30_capture_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
#define S30_TRACE(var) double var = s30_capture_ms()
#else
#define S30_TRACE(var)
#endif

static inline int s30_capture(const S30Capture* c, int gameOnly, int vsync, uint32_t* screen) {
	static S30CapturePlane planes[16];
	drmModePlaneRes* res = drmModeGetPlaneResources(c->fd);
	uint32_t order[16];
	uint64_t zpos[16];
	int i, j, n = 0, ok[16];
	S30_TRACE(t0);

	memset(screen, 0, sizeof(uint32_t) * PANEL_W * PANEL_H);
	for (i = 0; res != NULL && i < (int)res->count_planes && n < 16; i++) {
		drmModePlane* p = drmModeGetPlane(c->fd, res->planes[i]);

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
	drmModeFreePlaneResources(res);
	S30_TRACE(t1);
	for (i = 0; i < n; i++) {
		ok[i] = s30_capture_prepare(c->fd, order[i], &planes[i]) == 0;
	}
	S30_TRACE(t2);
	for (j = 0;; j++) {
		int stable = 1;

		if (vsync) {
			drmVBlank vb;

			memset(&vb, 0, sizeof(vb));
			vb.request.type = DRM_VBLANK_RELATIVE;
			vb.request.sequence = 1;
			drmWaitVBlank(c->fd, &vb);
			for (i = 0; i < n; i++) {
				if (ok[i]) {
					ok[i] = s30_capture_rebuffer(c->fd, order[i], &planes[i]) == 0;
				}
			}
		}
		for (i = 0; i < n; i++) {
			if (ok[i]) {
				ok[i] = s30_capture_copy(&planes[i]) == 0;
			}
		}
		if (!vsync) {
			break;
		}
		for (i = 0; i < n; i++) {
			if (ok[i] && s30_capture_fb(c->fd, order[i]) != planes[i].fbId) {
				stable = 0; // flipped during the copy
			}
		}
		if (stable || j == S30_CAPTURE_RETRIES) {
			break;
		}
	}
	S30_TRACE(t3);
	S30_TRACE(t4);
	S30_TRACE(t5);
	for (i = 0; i < n; i++) {
		s30_capture_unmap(c->fd, &planes[i]);
	}
	for (i = 0; i < n; i++) {
		if (ok[i]) {
			s30_capture_blend(&planes[i], screen);
		}
	}
#ifdef S30_CAPTURE_TRACE
	fprintf(stderr, "capture: %d planes, list %.1f prepare %.1f vblank+copy %.1f (%d tries) blend %.1f ms\n", n,
	        t1 - t0, t2 - t1, t3 - t2, j + 1, s30_capture_ms() - t5);
#endif
	return 0;
}

#endif
