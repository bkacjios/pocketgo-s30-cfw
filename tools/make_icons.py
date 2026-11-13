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
import struct
import sys
import zlib

from PIL import Image

SIZE = 128
PAD = 4

OOT = "ports/oot/extracted/gc-us/assets/objects/object_mag"
MM = "ports/mm/extracted/n64-us/assets/objects/object_mag"
DKR = "ports/dkr/assets/.vanilla/us.v77/textures/2d/menu"
MK64 = "ports/mk64/textures/standalone/logo_mario_kart_64.rgba32.png"  # the title screen's logo
RS64_ROM = "ports/rogue_squadron64/rogue_squadron.z64"  # its title logo is backdrop/logo in the ROM's asset blob
SF64_ROM = "ports/sf64/baserom.us.rev1.uncompressed.z64"  # title logo in ast_title (assets/yaml/us/rev1/ast_title.yaml)
RETROARCH = "s30-build/rootfs/usr/share/libretro/assets/branding/retroarch-plain-logo.png"

# Screenshot crops: app name -> (screenshot file, (left, top, right, bottom))
SHOTS = {
}


def rs64_asset(rom, folder, name):
    """A Rogue Squadron asset by folder and name: the blob at ROM 0x144340
    (ports/rogue_squadron64/docs/data_blob), zlib unless stored raw"""
    for seg in range(2):
        base = 0x144380 + struct.unpack(">I", rom[0x144340 + seg * 0x20 + 0x1C:][:4])[0]
        manifest_off, _, manifest_size = struct.unpack(">IHH", rom[base:base + 8])
        entries = [rom[base + manifest_off + i * 0x20:][:0x20] for i in range(manifest_size // 0x20)]
        current = None
        for e in entries:
            off, size, csize, flags = struct.unpack(">IIIB", e[:13])
            ename = e[16:].split(b"\0")[0].decode("latin1")
            if flags & 0x80:
                current = ename
            elif current == folder and ename == name:
                data = rom[base + off:][:size if csize == 0xFFFFFFFF else csize]
                return data if csize == 0xFFFFFFFF else zlib.decompress(data)
    return None


def rs64_ci8(data):
    """A partial-header CI8 image (type 1): 12-byte header, 256 RGBA
    colours, then the pixels"""
    w, h = struct.unpack(">HH", data[4:8])
    pal = [tuple(data[12 + i * 4:16 + i * 4]) for i in range(256)]
    im = Image.new("RGBA", (w, h))
    im.putdata([pal[p] for p in data[12 + 1024:12 + 1024 + w * h]])
    return im


def sf64_title(rom):
    """Star Fox 64's title: aTitleStarfoxLogoTex (RGBA16 236x60) over the
    "64", seven CI8 32x64 pieces of aTitle64LogoDL's quads (a 4x2 grid, the
    top right cell empty)"""
    seg = lambda addr: 0xBFC510 + addr - 0x6000000  # ast_title, segment 6

    def rgba16(addr, n):
        out = []
        for i in range(n):
            v = struct.unpack(">H", rom[seg(addr) + i * 2:][:2])[0]
            out.append(((v >> 11 & 31) * 255 // 31, (v >> 6 & 31) * 255 // 31, (v >> 1 & 31) * 255 // 31, 255 * (v & 1)))
        return out

    logo = Image.new("RGBA", (236, 60))
    logo.putdata(rgba16(0x6000000, 236 * 60))
    # (texture, TLUT, colours, height, grid column, grid row)
    pieces = [(0x6014DE8, 0x60155E8, 152, 64, 0, 0), (0x6015718, 0x6015EF8, 64, 63, 0, 1),
              (0x6015F78, 0x6016778, 200, 64, 1, 0), (0x6016908, 0x60170E8, 144, 63, 1, 1),
              (0x6017208, 0x6017A08, 136, 64, 2, 0), (0x6017B18, 0x6018318, 200, 64, 2, 1),
              (0x60184A8, 0x6018CA8, 76, 64, 3, 1)]
    six4 = Image.new("RGBA", (128, 128))
    for tex, tlut, colours, h, col, row in pieces:
        pal = rgba16(tlut, colours)
        piece = Image.new("RGBA", (32, h))
        piece.putdata([pal[i] if i < colours else (0, 0, 0, 0) for i in rom[seg(tex):][:32 * h]])
        six4.paste(piece, (32 * col, 64 * row))
    logo, six4 = trim(logo), trim(six4)
    h = int(logo.height * 1.7)
    six4 = six4.resize((six4.width * h // six4.height, h), Image.LANCZOS)
    out = Image.new("RGBA", (max(logo.width, six4.width), logo.height + h - 8))
    out.alpha_composite(six4, ((out.width - six4.width) // 2, logo.height - 8))
    out.alpha_composite(logo, ((out.width - logo.width) // 2, 0))
    return out


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
    if os.path.exists(MK64):
        icons["Mario Kart 64"] = fit(trim(Image.open(MK64)))
    if os.path.exists(RS64_ROM):
        logo = rs64_asset(open(RS64_ROM, "rb").read(), "backdrop", "logo")
        if logo is not None:
            icons["Rogue Squadron"] = fit(trim(rs64_ci8(logo)))
    if os.path.exists(SF64_ROM):
        icons["Star Fox 64"] = fit(sf64_title(open(SF64_ROM, "rb").read()))
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
