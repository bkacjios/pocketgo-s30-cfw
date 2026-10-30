#!/usr/bin/env python3
"""Bring up the A33 MIPI DSI host + D-PHY over FEL and send DCS commands to
the ILI9488 in low power mode, without U-Boot. For debugging the U-Boot DSI
driver: register values mirror drivers/video/sunxi/sun6i_dsi.c.

Usage: fel_dsi_test.py <u-boot-sunxi-with-spl.bin> [--dphy-clk linux|stock]
       [--no-spl] [cmd ...]

The SPL is run first (PMIC rails incl. panel power, PLL6). Each cmd is DCS
bytes in hex, e.g. 11 29 22 sends sleep out, display on, all pixels off.
"""

import argparse
import subprocess
import sys
import time

CCM = 0x01c20000
DSI = 0x01ca0000
DPHY = 0x01ca1000
PIO = 0x01c20800
RTC_GP = 0x01f00100


def fel(*args):
    return subprocess.run(["sunxi-fel", *args], check=True,
                          capture_output=True, text=True).stdout


def w(addr, val):
    fel("writel", hex(addr), hex(val & 0xffffffff))


def r(addr):
    return int(fel("readl", hex(addr)).strip(), 16)


def setbits(addr, bits):
    w(addr, r(addr) | bits)


ECC = [
    0b111100010010110010110111, 0b111100100101010101011011,
    0b011101001001101001101101, 0b101110001110001110001110,
    0b110111110000001111110000, 0b111011111111110000000000,
]


def ecc(d):
    return sum((bin(d & m).count("1") & 1) << i for i, m in enumerate(ECC))


def hdr(dt, d0=0, d1=0):
    v = (dt & 0x3f) | d0 << 8 | d1 << 16
    return v | ecc(v) << 24


def crc(data):
    c = 0xffff
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ (0x8408 if c & 1 else 0)
    return c


def inst_wait():
    for _ in range(50):
        if not r(DSI + 0x10) & 1:
            return True
        time.sleep(0.001)
    return False


def dcs(data):
    if not inst_wait():
        print("  busy before send")
        w(DSI + 0x10, r(DSI + 0x10) & ~1)
    w(DSI + 0x200, 1 << 26 | 1 << 25 | 1 << 9)
    if len(data) <= 2:
        dt = 0x05 if len(data) == 1 else 0x15
        w(DSI + 0x300, hdr(dt, data[0], data[1] if len(data) > 1 else 0))
        w(DSI + 0x200, (r(DSI + 0x200) & ~0xff) | 3)
    else:
        w(DSI + 0x300, hdr(0x39, len(data) & 0xff, len(data) >> 8))
        c = crc(data)
        buf = bytes(data) + bytes([c & 0xff, c >> 8])
        buf += bytes(-len(buf) % 4)
        for i in range(0, len(buf), 4):
            w(DSI + 0x304 + i, int.from_bytes(buf[i:i + 4], "little"))
        w(DSI + 0x200, len(data) + 2 + 4 - 1)
    # LP11 -> LPDT -> END
    w(DSI + 0x48, 4 << 0 | 15 << 16)
    w(DSI + 0x10, r(DSI + 0x10) & ~1)
    w(DSI + 0x10, r(DSI + 0x10) | 1)
    done = inst_wait()
    print(f"  dcs {bytes(data).hex(' ')}: done={done} "
          f"CMD_CTL={r(DSI + 0x200):#010x}")


def dsi_init(dphy_clk):
    # PLL3 = 24 MHz * 99 / 8 = 297 MHz, integer mode
    w(CCM + 0x010, 1 << 31 | 1 << 24 | 98 << 8 | 7)
    time.sleep(0.01)
    if dphy_clk == "stock":
        w(CCM + 0x168, 0x85008000)  # as read from the stock kernel
    else:
        # sclk PLL3 /1, D-PHY PLL6 /4 = 150 MHz (Linux, U-Boot driver)
        w(CCM + 0x168, 1 << 31 | 1 << 15 | 2 << 8 | 3)
    setbits(CCM + 0x060, 1 << 1)
    setbits(CCM + 0x2c0, 1 << 1)

    w(DSI + 0x000, 1)
    w(DSI + 0x010, 1 << 16 | 1 << 17)
    w(DSI + 0x060, 10)
    w(DSI + 0x078, 0)
    inst = [0x00000011, 0x10000001, 0x20000010, 0x20000001,
            0x30100001, 0x40000010, 0x00000001, 0x50000011]
    for i, v in enumerate(inst):
        w(DSI + 0x20 + 4 * i, v)
    w(DSI + 0x4c, 0x00560001)
    w(DSI + 0x2f8, 0xff)

    p = DPHY
    w(p + 0x04, 1 << 28)
    w(p + 0x10, 0x0a06000e)
    w(p + 0x14, 0x0a033207)
    w(p + 0x18, 0x1e)
    w(p + 0x1c, 0)
    w(p + 0x20, 0x303)
    w(p + 0x4c, 0x91007100)
    w(p + 0x50, 0x17000000)
    w(p + 0x5c, 0x01101555)
    w(p + 0x54, 0x00000002)
    time.sleep(0.001)
    w(p + 0x58, 0x03040000)
    setbits(p + 0x58, 1 << 28 | 1 << 27)
    setbits(p + 0x58, 1 << 26)
    setbits(p + 0x54, 1 << 4)
    setbits(p + 0x50, 1 << 31)
    setbits(p + 0x54, 1 << 24)
    w(p + 0x00, 1)


def panel_reset():
    # PD26 output: PD_CFG3 bits 10:8, PD_DAT bit 26
    cfg = PIO + 3 * 0x24 + 0x0c
    dat = PIO + 3 * 0x24 + 0x10
    w(cfg, (r(cfg) & ~(7 << 8)) | 1 << 8)
    for val, ms in ((1, 50), (0, 20), (1, 100)):
        w(dat, (r(dat) & ~(1 << 26)) | val << 26)
        time.sleep(ms / 1000)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("spl")
    ap.add_argument("--dphy-clk", choices=("linux", "stock"), default="linux")
    ap.add_argument("--no-spl", action="store_true")
    ap.add_argument("cmds", nargs="*", default=["11", "29", "22"])
    a = ap.parse_args()

    if not a.no_spl:
        fel("spl", a.spl)
    dsi_init(a.dphy_clk)
    print(f"CCM 0x168={r(CCM + 0x168):#010x} DSI CTL0={r(DSI + 0x10):#010x} "
          f"DPHY GCTL={r(DPHY):#010x}")
    panel_reset()
    for c in a.cmds:
        data = list(bytes.fromhex(c))
        dcs(data)
        if data[0] == 0x11:
            time.sleep(0.12)
    return 0


if __name__ == "__main__":
    sys.exit(main())
