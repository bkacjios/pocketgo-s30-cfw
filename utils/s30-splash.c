/*
 * s30-splash: boot splash on the framebuffer console (/dev/fb0): the U-Boot
 * splash image with a progress bar under the logo.
 *
 * Usage: s30-splash PERCENT
 *        s30-splash -i IMAGE   (another BMP of the same kind, no bar)
 *
 * The image is the 24-bit BMP U-Boot shows (/usr/share/s30/splash.bmp), in
 * panel-native portrait orientation (320x480). The device is held in
 * landscape with the panel's +x pointing down, so "under the logo" is a
 * vertical strip at higher x, filling from the bottom of the panel (the left
 * side as held) upwards.
 */
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define SPLASH_BMP "/usr/share/s30/splash.bmp"

// Bar in panel coordinates (logo spans x 55-195, y 102-378)
#define BAR_X 222
#define BAR_W 6
#define BAR_Y0 140
#define BAR_Y1 340

static const uint8_t kTrack[3] = { 40, 40, 40 };
static const uint8_t kFill[3] = { 124, 104, 190 }; // the purple of "Go"

static struct fb_var_screeninfo sVar;
static struct fb_fix_screeninfo sFix;
static uint8_t *sFb;

static void put_pixel(int x, int y, const uint8_t rgb[3]) {
	uint8_t *p;

	if (x < 0 || y < 0 || x >= (int)sVar.xres || y >= (int)sVar.yres) {
		return;
	}
	p = sFb + y * sFix.line_length + x * (sVar.bits_per_pixel / 8);
	if (sVar.bits_per_pixel == 32) {
		uint32_t v = (rgb[0] >> (8 - sVar.red.length)) << sVar.red.offset |
			     (rgb[1] >> (8 - sVar.green.length)) << sVar.green.offset |
			     (rgb[2] >> (8 - sVar.blue.length)) << sVar.blue.offset;
		memcpy(p, &v, 4);
	} else if (sVar.bits_per_pixel == 16) {
		uint16_t v = (rgb[0] >> 3) << 11 | (rgb[1] >> 2) << 5 | rgb[2] >> 3;
		memcpy(p, &v, 2);
	}
}

/* 24-bit uncompressed BMP, bottom-up rows padded to 4 bytes */
static void draw_bmp(const char *path) {
	FILE *f = fopen(path, "rb");
	uint8_t hdr[54], *row;
	int32_t w, h, offset;
	int stride, x, y;

	if (f == NULL) {
		return;
	}
	if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || hdr[0] != 'B' || hdr[1] != 'M' ||
	    hdr[28] != 24) {
		fclose(f);
		return;
	}
	memcpy(&offset, hdr + 10, 4);
	memcpy(&w, hdr + 18, 4);
	memcpy(&h, hdr + 22, 4);
	stride = (w * 3 + 3) & ~3;
	row = malloc(stride);
	if (row == NULL || fseek(f, offset, SEEK_SET) != 0) {
		free(row);
		fclose(f);
		return;
	}
	for (y = 0; y < abs(h); y++) {
		if (fread(row, 1, stride, f) != (size_t)stride) {
			break;
		}
		for (x = 0; x < w; x++) {
			uint8_t rgb[3] = { row[x * 3 + 2], row[x * 3 + 1], row[x * 3] };
			put_pixel(x, h > 0 ? h - 1 - y : y, rgb);
		}
	}
	free(row);
	fclose(f);
}

int main(int argc, char **argv) {
	int fd, percent = 0, fill, x, y;
	const char *image = NULL;

	if (argc == 3 && strcmp(argv[1], "-i") == 0) {
		image = argv[2];
	} else if (argc == 2) {
		percent = atoi(argv[1]);
		percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	} else {
		fprintf(stderr, "usage: s30-splash PERCENT | s30-splash -i IMAGE\n");
		return 2;
	}

	fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &sVar) < 0 ||
	    ioctl(fd, FBIOGET_FSCREENINFO, &sFix) < 0) {
		perror("s30-splash: /dev/fb0");
		return 1;
	}
	sFb = mmap(NULL, sFix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (sFb == MAP_FAILED) {
		perror("s30-splash: mmap");
		return 1;
	}

	if (image != NULL) {
		draw_bmp(image);
		return 0;
	}
	draw_bmp(SPLASH_BMP);
	fill = BAR_Y1 - (BAR_Y1 - BAR_Y0) * percent / 100;
	for (y = BAR_Y0; y < BAR_Y1; y++) {
		for (x = BAR_X; x < BAR_X + BAR_W; x++) {
			put_pixel(x, y, y >= fill ? kFill : kTrack);
		}
	}
	return 0;
}
