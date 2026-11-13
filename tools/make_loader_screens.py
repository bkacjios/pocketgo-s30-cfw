#!/usr/bin/env python3
"""Make the s30-loader screens: the boot splash (build/splash/logo.bmp) with
text under the logo.

  build/splash/usb.bmp     USB disk mode
  build/splash/nocard.bmp  no SD card: s30-loader waits for one

Text uses the kernel's VGA 8x16 console font (lib/fonts/font_8x16.c from the
kernel tree build.sh clones), scaled up without smoothing to match the
pixel-art logo. The BMP is panel-native portrait (320x480); the device is
held landscape with the panel's +x pointing down, so the text is drawn on a
landscape canvas and rotated back.

Usage: python3 tools/make_loader_screens.py [font_8x16.c]
"""
import re
import sys
from pathlib import Path

from PIL import Image

PROJECT = Path(__file__).resolve().parent.parent
SPLASH = PROJECT / "build" / "splash"
FONT = Path(sys.argv[1]) if len(sys.argv) > 1 else \
    PROJECT / "s30-build" / "linux" / "lib" / "fonts" / "font_8x16.c"
GLYPH_H = 16

# Landscape coordinates (480x320); the logo ends at y ~200
WHITE, PURPLE = (255, 255, 255), (160, 140, 230)  # "Go" purple, lighter
SCREENS = {  # file: [(text, scale, centre y, rgb)]
    "usb.bmp": [
        ("USB Storage Device", 2, 240, WHITE),
        ("Eject on the PC or press START", 1, 284, PURPLE),
    ],
    "nocard.bmp": [
        ("No SD Card", 2, 240, WHITE),
        ("Insert a card to continue", 1, 284, PURPLE),
    ],
}

# One byte per glyph row, a line each ("\t0x18, /* 00011000 */")
rows = [int(b, 16) for b in re.findall(r"^\s*(0x[0-9a-fA-F]{2}),", FONT.read_text(), re.M)]
assert len(rows) == 256 * GLYPH_H, f"{FONT}: {len(rows)} rows, expected {256 * GLYPH_H}"


def draw_text(img, text, scale, cy, rgb):
    x0 = (img.width - len(text) * 8 * scale) // 2
    y0 = cy - GLYPH_H * scale // 2
    for i, ch in enumerate(text.encode("ascii")):
        for gy in range(GLYPH_H):
            bits = rows[ch * GLYPH_H + gy]
            for gx in range(8):
                if bits & (0x80 >> gx):
                    x, y = x0 + (i * 8 + gx) * scale, y0 + gy * scale
                    img.paste(rgb, (x, y, x + scale, y + scale))


logo = Image.open(SPLASH / "logo.bmp").convert("RGB").transpose(Image.Transpose.ROTATE_270)
for name, lines in SCREENS.items():
    land = logo.copy()
    for line in lines:
        draw_text(land, *line)
    land.transpose(Image.Transpose.ROTATE_90).save(SPLASH / name)
