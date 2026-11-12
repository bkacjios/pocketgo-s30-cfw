/*
 * s30-screenshot: the screen as shown, from the display's KMS planes, to a
 * PNG (or a PPM, by the file's extension) in landscape orientation (480x320).
 *
 * The planes are read and blended by s30-capture.h (root only). Nothing has
 * to cooperate: it works over the launcher, a game or s30-display's menu.
 *
 * Usage: s30-screenshot [-g] [OUT.png|OUT.ppm]   (default /tmp/screenshot.png)
 *   -g: the game only, the primary plane (no OSD, menu or performance overlay)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "s30-capture.h"

/* A PNG chunk: length, type and data, then the CRC of type and data */
static void png_chunk(FILE* f, const char* type, const uint8_t* data, uint32_t n) {
	uint8_t be[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
	uLong crc = crc32(0, (const Bytef*)type, 4);

	if (n != 0) {
		crc = crc32(crc, data, n);
	}
	fwrite(be, 1, 4, f);
	fwrite(type, 1, 4, f);
	fwrite(data, 1, n, f);
	be[0] = (uint8_t)(crc >> 24);
	be[1] = (uint8_t)(crc >> 16);
	be[2] = (uint8_t)(crc >> 8);
	be[3] = (uint8_t)crc;
	fwrite(be, 1, 4, f);
}

/* 8-bit RGB rows (w x h), each with filter byte 0, deflated */
static int write_png(FILE* f, const uint8_t* rgb, int w, int h) {
	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	uint8_t ihdr[13] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
	                     (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
	                     8, 2, 0, 0, 0 };
	uLong rawLen = (uLong)(w * 3 + 1) * h, zLen = compressBound(rawLen);
	uint8_t* raw = malloc(rawLen);
	uint8_t* z = malloc(zLen);
	int y, ok;

	if (raw == NULL || z == NULL) {
		free(raw);
		free(z);
		return -1;
	}
	for (y = 0; y < h; y++) {
		raw[(size_t)y * (w * 3 + 1)] = 0;
		memcpy(&raw[(size_t)y * (w * 3 + 1) + 1], &rgb[(size_t)y * w * 3], (size_t)w * 3);
	}
	ok = compress2(z, &zLen, raw, rawLen, 6) == Z_OK;
	if (ok) {
		fwrite(sig, 1, 8, f);
		png_chunk(f, "IHDR", ihdr, 13);
		png_chunk(f, "IDAT", z, (uint32_t)zLen);
		png_chunk(f, "IEND", NULL, 0);
	}
	free(raw);
	free(z);
	return ok ? 0 : -1;
}

int main(int argc, char** argv) {
	static uint32_t screen[PANEL_W * PANEL_H];
	static uint8_t rgb[PANEL_W * PANEL_H * 3];
	int gameOnly = argc > 1 && strcmp(argv[1], "-g") == 0;
	const char* out = argc > 1 + gameOnly ? argv[1 + gameOnly] : "/tmp/screenshot.png";
	size_t outLen = strlen(out);
	int png = outLen < 4 || strcmp(out + outLen - 4, ".ppm") != 0;
	S30Capture cap;
	int x, y;
	FILE* f;

	if (s30_capture_open(&cap) != 0) {
		return 1;
	}
	s30_capture(&cap, gameOnly, 0, screen); // the app is stopped under the menu
	f = fopen(out, "wb");
	if (f == NULL) {
		perror(out);
		return 1;
	}
	for (y = 0; y < PANEL_W; y++) {
		for (x = 0; x < PANEL_H; x++) {
			uint32_t c = screen[(PANEL_H - 1 - x) * PANEL_W + y];
			uint8_t* d = &rgb[((size_t)y * PANEL_H + x) * 3];

			d[0] = (uint8_t)(c >> 16);
			d[1] = (uint8_t)(c >> 8);
			d[2] = (uint8_t)c;
		}
	}
	if (png) {
		if (write_png(f, rgb, PANEL_H, PANEL_W) != 0) {
			fprintf(stderr, "s30-screenshot: png encoding failed\n");
			fclose(f);
			return 1;
		}
	} else {
		fprintf(f, "P6\n%d %d\n255\n", PANEL_H, PANEL_W);
		fwrite(rgb, 1, sizeof(rgb), f);
	}
	fclose(f);
	return 0;
}
