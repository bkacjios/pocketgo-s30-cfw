#!/usr/bin/env python3
"""Launcher icons (128x128 PNG, transparent) for the apps on the card, from
the ports' extracted game assets and from title-screen screenshots. The art
is the games' own, so the icons live on the card (/mnt/sd/apps/NAME.png,
beside NAME.sh), not in this repository.

Usage: make_icons.py OUTDIR [SHOT_DIR]
  OUTDIR    where the PNGs go (copy them to /mnt/sd/apps)
  SHOT_DIR  screenshots (s30-screenshot, 480x320) for the games whose logo
            is not a texture: NAME.png per entry in SHOTS, cropped by box

Needs Pillow. Run from the repository root.
"""
import os
import sys

from PIL import Image

SIZE = 128
PAD = 4

OOT = "ports/oot/extracted/gc-us/assets/objects/object_mag"
MM = "ports/mm/extracted/n64-us/assets/objects/object_mag"
DKR = "ports/dkr/assets/.vanilla/us.v77/textures/2d/menu"
RETROARCH = "s30-build/rootfs/usr/share/libretro/assets/branding/retroarch-plain-logo.png"

# Screenshot crops: app name -> (screenshot file, (left, top, right, bottom))
SHOTS = {
}


def fit(im, box=SIZE - 2 * PAD):
    """`im` scaled to fit a box, centred on a transparent SIZE x SIZE canvas"""
    im = im.convert("RGBA")
    scale = min(box / im.width, box / im.height)
    if scale != 1:
        im = im.resize((max(1, round(im.width * scale)), max(1, round(im.height * scale))), Image.LANCZOS)
    out = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    out.alpha_composite(im, ((SIZE - im.width) // 2, (SIZE - im.height) // 2))
    return out


def trim(im):
    """Cut transparent borders"""
    im = im.convert("RGBA")
    bbox = im.getchannel("A").getbbox()
    return im.crop(bbox) if bbox else im


def strip(files):
    """Tiles side by side (DKR's title logo is ten 16x64 strips)"""
    ims = [Image.open(f).convert("RGBA") for f in files]
    out = Image.new("RGBA", (sum(i.width for i in ims), max(i.height for i in ims)))
    x = 0
    for im in ims:
        out.alpha_composite(im, (x, 0))
        x += im.width
    return out


def rounded(im, radius=18):
    """Screenshot crops get rounded corners (the logo keeps its background)"""
    from PIL import ImageDraw
    mask = Image.new("L", im.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, im.width - 1, im.height - 1), radius=radius, fill=255)
    im = im.convert("RGBA")
    im.putalpha(mask)
    return im


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    shots = sys.argv[2] if len(sys.argv) > 2 else None
    os.makedirs(out, exist_ok=True)
    icons = {}
    if os.path.exists(OOT):
        icons["Ocarina of Time"] = fit(trim(Image.open(f"{OOT}/gTitleZeldaShieldLogoTex.rgba32.png")))
    if os.path.exists(MM):
        icons["Majora's Mask"] = fit(trim(Image.open(f"{MM}/title_screen_majoras_mask.rgba32.png")))
    if os.path.exists(DKR):
        icons["Diddy Kong Racing"] = fit(trim(strip([f"{DKR}/title_{i}.png" for i in range(10)])))
    if os.path.exists(RETROARCH):
        logo = Image.open(RETROARCH).crop((0, 0, 460, 330))  # the invader, not the wordmark or its rule
        icons["RetroArch"] = fit(trim(logo))
    for name, (shot, box) in SHOTS.items():
        path = os.path.join(shots or "", shot)
        if shots is not None and os.path.exists(path):
            im = Image.open(path).convert("RGBA").crop(box)
            icons[name] = fit(rounded(im))
    for name, im in icons.items():
        im.save(os.path.join(out, f"{name}.png"))
        print(name)


if __name__ == "__main__":
    main()
