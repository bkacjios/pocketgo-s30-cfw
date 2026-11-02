/*
 * s30-screenshot: the screen as shown, from the display's KMS planes, to a
 * PPM in landscape orientation (480x320).
 *
 * Every plane on the panel's CRTC with a framebuffer is read through GETFB2
 * (root: the buffer handles need CAP_SYS_ADMIN), mapped as a dumb buffer and
 * blended in zpos order, the alpha formats over what is below. Nothing has
 * to cooperate: it works over the launcher, a game or s30-display's menu.
 *
 * Usage: s30-screenshot [OUT.ppm]   (default /tmp/screenshot.ppm)
 */
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

static uint64_t prop(int fd, uint32_t plane, const char* name, uint64_t def) {
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

/* One plane's pixels blended into `screen` (panel layout, 0xRRGGBB) */
static int blend_plane(int fd, uint32_t plane, uint32_t* screen) {
	drmModePlane* pl = drmModeGetPlane(fd, plane);
	drmModeFB2* fb;
	struct drm_mode_map_dumb map = { 0 };
	int cx, cy, cw, ch, sx, sy, x, y, alpha, bpp;
	uint8_t* mem;
	size_t size;

	if (pl == NULL || pl->fb_id == 0) {
		drmModeFreePlane(pl);
		return 0;
	}
	fb = drmModeGetFB2(fd, pl->fb_id);
	drmModeFreePlane(pl);
	if (fb == NULL || fb->handles[0] == 0) {
		fprintf(stderr, "s30-screenshot: plane %u: no buffer handle (not root?)\n", plane);
		drmModeFreeFB2(fb);
		return -1;
	}
	switch (fb->pixel_format) {
		case DRM_FORMAT_XRGB8888:
		case DRM_FORMAT_ARGB8888:
			bpp = 4;
			break;
		case DRM_FORMAT_RGB565:
			bpp = 2;
			break;
		default:
			fprintf(stderr, "s30-screenshot: plane %u: format %.4s not handled\n", plane,
			        (const char*)&fb->pixel_format);
			drmModeFreeFB2(fb);
			return -1;
	}
	alpha = fb->pixel_format == DRM_FORMAT_ARGB8888;
	map.handle = fb->handles[0];
	size = (size_t)fb->pitches[0] * fb->height + fb->offsets[0];
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0 ||
	    (mem = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, map.offset)) == MAP_FAILED) {
		perror("s30-screenshot: map");
		drmModeFreeFB2(fb);
		return -1;
	}
	// Where it shows (CRTC_*) and what part of the buffer (SRC_*, 16.16)
	cx = (int)(int64_t)prop(fd, plane, "CRTC_X", 0);
	cy = (int)(int64_t)prop(fd, plane, "CRTC_Y", 0);
	cw = (int)prop(fd, plane, "CRTC_W", fb->width);
	ch = (int)prop(fd, plane, "CRTC_H", fb->height);
	sx = (int)(prop(fd, plane, "SRC_X", 0) >> 16);
	sy = (int)(prop(fd, plane, "SRC_Y", 0) >> 16);
	for (y = 0; y < ch; y++) {
		for (x = 0; x < cw; x++) {
			const uint8_t* s = mem + fb->offsets[0] + (size_t)(sy + y) * fb->pitches[0] + (size_t)(sx + x) * bpp;
			uint32_t* d;
			uint32_t c, a;

			if (cx + x < 0 || cx + x >= PANEL_W || cy + y < 0 || cy + y >= PANEL_H || sx + x >= (int)fb->width ||
			    sy + y >= (int)fb->height) {
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
	drmModeFreeFB2(fb);
	return 0;
}

int main(int argc, char** argv) {
	static uint32_t screen[PANEL_W * PANEL_H];
	const char* out = argc > 1 ? argv[1] : "/tmp/screenshot.ppm";
	drmModeRes* res = NULL;
	drmModePlaneRes* planes;
	uint32_t crtc = 0, order[16];
	uint64_t zpos[16];
	int fd = -1, i, j, n = 0, x, y;
	FILE* f;

	for (i = 0; i < 4 && res == NULL; i++) { // the KMS card (the GPU has its own)
		char path[32];

		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd >= 0 && (res = drmModeGetResources(fd)) != NULL && res->count_crtcs > 0) {
			break;
		}
		drmModeFreeResources(res);
		res = NULL;
		if (fd >= 0) {
			close(fd);
		}
	}
	if (res == NULL) {
		fprintf(stderr, "s30-screenshot: no display card\n");
		return 1;
	}
	for (i = 0; i < res->count_crtcs && crtc == 0; i++) {
		drmModeCrtc* c = drmModeGetCrtc(fd, res->crtcs[i]);

		if (c != NULL && c->mode_valid) {
			crtc = c->crtc_id;
		}
		drmModeFreeCrtc(c);
	}
	drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	planes = drmModeGetPlaneResources(fd);
	for (i = 0; planes != NULL && i < (int)planes->count_planes && n < 16; i++) {
		drmModePlane* p = drmModeGetPlane(fd, planes->planes[i]);

		if (p != NULL && p->crtc_id == crtc && p->fb_id != 0) {
			// Sorted by zpos as found
			uint64_t z = prop(fd, p->plane_id, "zpos", (uint64_t)i);
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
	for (i = 0; i < n; i++) {
		blend_plane(fd, order[i], screen);
	}
	f = fopen(out, "wb");
	if (f == NULL) {
		perror(out);
		return 1;
	}
	fprintf(f, "P6\n%d %d\n255\n", PANEL_H, PANEL_W);
	for (y = 0; y < PANEL_W; y++) {
		for (x = 0; x < PANEL_H; x++) {
			uint32_t c = screen[(PANEL_H - 1 - x) * PANEL_W + y];
			unsigned char rgb[3] = { (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c };
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
	return 0;
}
