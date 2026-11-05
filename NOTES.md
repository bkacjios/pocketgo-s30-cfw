# PocketGo S30 mainline Linux: project notes

Handoff notes for continuing this work. Last updated: 2026-10-30.

## Goal

A RetroArch handheld on mainline Linux + U-Boot, booting from the SD card and
leaving the stock firmware in SPI flash untouched (remove the card = stock).

## Status (2026-10-30)

- Boots standalone from SD (verified on a FAT32 card): U-Boot splash (stock
  PocketGo logo) -> kernel logo -> splash with progress bar -> launcher
  (s30-launcher: RetroArch, Ocarina of Time, Super Mario 64; power key =
  system menu, with the joystick calibration under System). See "Standalone SD card
  boot" and "Linux system".
- Since 2026-10-29: BusyBox init (rootfs-overlay), kernel-owned headphone
  jack, system volume daemon, boot splash, quieter amp pops, OoT audio fixed
  (voices, sfx, drums, gain), SM64 port.
- Working: DSI display (U-Boot + Linux), GPU (lima, GLES2), buttons + analog
  stick (own input driver), speaker (mono) / headphones (stereo) with jack
  detect, blue status LED, USB serial shell, RetroArch (own build) with
  rotation, menu, input, audio, volume keys.
- **Pending (next session):** test exFAT. U-Boot exFAT fix (patch 0003) is
  built but untested on hardware. Card currently FAT32 and working; to test:
  back up `home/roms`, `sudo bash build/build.sh sdcard /dev/sdX format`
  (erases the card, makes exFAT), restore ROMs, boot. Also test USB disk
  mode (hold SELECT at power on; see USB below).
- Open: red/green LED not found (likely charger hardware); stick click dead on the test unit; clean power-off (long press
  power -> RetroArch quits, sync) not done, so a hard power-off can lose
  unsaved RetroArch config; more cores (pcsx_rearmed, snes9x, picodrive)
  and EmulationStation not built.

## Project layout

```
S30-info/                 Original dump (git: github.com/bkacjios/S30-info; not in
                          this repo)
u-boot/                   Old U-Boot v2022.01 checkout (reference only, NOT built,
                          not in this repo)
external/                 Submodules, pinned: linux (v7.3), u-boot (v2026.10),
                          retroarch (v1.22.2). Built in worktrees under $WORK.
ports/oot/                Submodule: Ocarina of Time port (bkacjios/oot-s30).
ports/mm/                 Submodule: Majora's Mask port (bkacjios/mm-s30).
ports/tmc/                Submodule: The Minish Cap PC port (bkacjios/tmc),
                          `make pc-s30` (docs/pc-s30.md there).
ports/sf64/               Submodule: Star Fox 64 port (bkacjios/sf64-s30).
ports/mk64/               Submodule: Mario Kart 64 port (bkacjios/mk64-s30).
n64/                      Shared N64 platform library for ports/oot, mm, sf64, mk64
                          (display, renderer, audio, input; n64/README.md
                          has the API and the porting checklist).
                          Other ports/ checkouts are not in this repo.
s30-build/                $WORK: build trees (git worktrees of external/*,
                          patched there; sysroot) and outputs (boot/, rootfs.sqfs,
                          uInitrd, fel.scr, retroarch.bin, u-boot-sunxi-with-spl.bin)
firmware-decoded/         Output of tools/decode_firmware.py
tools/
  decode_firmware.py      Descrambles the dump into firmware-decoded/
  zklogo.py               Stock boot logo (p4, LZO1X) -> BMP
  apk_resolve.py          Alpine dependency resolver (APKINDEX, no apk)
  fel_axp_dump.sh         AXP223 (RSB) + CCU register dump from FEL
linux/
  arch/arm/boot/dts/allwinner/sun8i-a33-pocketgo-s30.dts
  drivers/gpu/drm/panel/  ILI9488 DSI panel driver (+Kconfig.s30/Makefile.s30)
  drivers/input/joystick/ pocketgo-s30-gamepad (MCU stick + GPIO buttons)
  drivers/video/logo/     logo_pocketgo_s30_clut224.ppm (kernel boot logo)
patches/                  Changes to the submodules, one folder each, applied
                          in order by build.sh in the worktrees:
  linux/0001              sun8i-codec-analog: mono differential switch,
                          headphone amp + HPCOM always on, owns the jack
                          (output switching on plug/unplug), volume control
                          named Master
  u-boot/                 0001 DSI LCD, 0002 splash env, 0003 exFAT on 32-bit
                          ARM, 0004 AXP settings (PEK restart, Voff,
                          CHGLED)
  retroarch/              0001 gl2 screen rotation, 0002 NEON asm syntax (clang)
  emulationstation/       0001 install rules + /usr/share resources (AUR),
                          0002 S30 input setup entries, no hotkey prompt
build/
  build.sh                deps|linux|uboot|rootfs|initramfs|retroarch|fel|sdcard
  kernel.config           Fragment on top of sunxi_defconfig
  packages                Alpine packages for the rootfs (!name excludes)
  s30-loader              initramfs /init (mounts SD, squashfs + overlay)
  rootfs-overlay/         Board files copied into the rootfs: /etc/inittab
                          (BusyBox init), /etc/s30/rc (setup), frontend,
                          default launcher apps, retroarch.cfg, autoconfig
  fel.cmd / fel-ram.cmd   Boot scripts (fel.scr is also /boot/boot.scr on SD)
  splash/logo.bmp         U-Boot splash (stock logo, 320x480 panel-native)
  u-boot/                 defconfig + board devicetree
launcher/                 Launcher (filebrowser.c)
utils/                    s30-fel (reboot into FEL), s30-volumed (volume keys,
                          jack), s30-display (DRM lease broker + OSD) with its
                          preload libs30lease.so
```

Regenerate decoded firmware: `python3 tools/decode_firmware.py S30-info/dump firmware-decoded`
(S30-info also carries the decoded images, the decoders and a README with
the hardware and stock firmware findings).

## Hardware (from script.fex + stock kernel)

| Item | Detail |
|---|---|
| SoC | Allwinner A33 (sun8iw5p1), stock board name `zkswe_a33` |
| DRAM | DDR3, 552 MHz, zq 0x3bfb, odt_en 1, tpr0 0x47214f, tpr1 0x1c2294b, tpr2 0x61043, tpr12 0xa8, tpr13 0x901 |
| PMIC | AXP223 on RSB (addr 0x3a3). dcdc1 3.0 V, dcdc2 1.1 V, dcdc3 1.2 V, dcdc5 1.5 V, aldo2 2.5 V, aldo3 3.0 V |
| Boot flash | 16 MB SPI NOR w25q128 on SPI0 (PC0-PC3) |
| SD card | mmc0 on PF0-PF5, card detect PB1 (active low) |
| Debug UART | UART1 on PG6 (TX) / PG7 (RX), 115200 |
| Backlight | PWM0 on PH0, 50 kHz, polarity 0 (active high, unverified) |
| Audio | Internal codec, PA enable PH9, headphone detect PL5 (fex "linein", low = plugged, verified) |
| USB | OTG, ID pin PL7, VBUS via AXP |
| Buttons | 21 GPIO keys, active low, polled 40 ms (list below) |

Buttons (fex gpkey1..17, mapped by probing pins on stock): up PG9, down PG8,
left PL8, right PL9, B PG10, Y PG11, A PG12, X PG13, select PL11, start PG4,
stick click? PG5 (broken on test unit), vol+ PG1, vol- PG0, L2 PB7, R1 PG3,
L1 PB6, R2 PG2. gpkey18-21 (PB5-PB2) read as pressed at rest on stock and
match no button; left out of the DTS. Stock reports the keys as
`gpio-keys-polled` codes 0x3a-0x4e in fex order.

Analog stick: MCU on I2C0 (PH2/PH3) at 0x60 (stock driver `mcu_ts`, address
list in the stock kernel). A plain 4 byte read returns A, ~A, B, ~B (further
bytes repeat the last one with bit 7 set); drop samples whose byte pairs do
not add up to 0xff (torn reads). Measured range X 30-235, Y 47-218, centre
~0x8f/0x85, noise +-1. The stick click does not show up anywhere (no GPIO
change, not in the MCU data) on the test unit. Mainline:
`linux/drivers/input/joystick/pocketgo-s30-gamepad.c` reports the stick and
all gamepad buttons as one input device "PocketGo S30 Gamepad"; gpio-keys
keeps only the volume keys. Axes as mounted: byte 0 is vertical (up = low),
byte 2 horizontal (right = high), so ABS_X = byte 2, ABS_Y = byte 0.

## Display

- Ilitek **ILI9488**, 320x480 portrait, mounted landscape.
- **MIPI DSI**, 1 lane, video mode, RGB888 (fex `lcd_if = 4`, `lcd_dsi_lane = 1`,
  `lcd_dsi_format = 0`, `lcd_dsi_if = 0`, `lcd_dsi_eotp = 0`).
- Timing: pclk 12 MHz, htotal 382, hbp 40 (incl. hspw 12), vtotal 524,
  vbp 22 (incl. vspw 4) -> 59.95 Hz. DRM mode: hfp 22 / hsync 12 / hbp 28,
  vfp 22 / vsync 4 / vbp 18.
- Power: AXP `dc1sw` (fex `lcd_power`) plus `dldo1` set to 3.3 V by the stock driver.
- Reset: PD26 (fex `lcd_gpio_0`). Stock sequence: pin cfg, 10 ms, high,
  50 ms, low, 20 ms, high, 100 ms, DSI HS clock on, 100 ms, init table.
- Init table (stock kernel `LCD_panel_init` @ 0xc01d3e9c, table @ 0xc09888ec):

```
dcs 0xe0 00 10 14 01 0e 04 33 56 48 03 0c 0b 2b 34 0f   positive gamma
dcs 0xe1 00 12 18 05 12 06 40 34 57 06 10 0c 3b 3f 0f   negative gamma
dcs 0xc0 0f 0c                                          power control 1
dcs 0xc1 41                                             power control 2
dcs 0xc5 00 26 80                                       VCOM
dcs 0x36 48                                             MADCTL
dcs 0x3a 66                                             COLMOD 18 bpp
dcs 0xb0 00
dcs 0xb1 a0                                             frame rate
dcs 0xb4 02                                             inversion control
dcs 0xb6 02 02                                          display function
dcs 0xe9 00
dcs 0x21                                                inversion on
dcs 0xf7 a9 51 2c 82                                    adjust control 3
dcs 0x11                                                sleep out
delay 120 ms
dcs 0x29                                                display on
delay 20 ms
```

- Mainline support: A33 DSI host is `sun6i_mipi_dsi` + `phy-sun6i-mipi-dphy`.
  Mainline has no ILI9488 DSI driver, hence `panel-ilitek-ili9488-dsi.c`
  (uses 6.15+ APIs: `devm_drm_panel_alloc`, `mipi_dsi_*_multi`).
- U-Boot: mainline has no sunxi DSI support, so `patches/u-boot/0001-*`
  adds it to the legacy `sunxi_display` (DE1) driver: TCON0 in DSI/CPU mode
  (`lcdc_tcon0_mode_set_dsi`), a DSI host + D-PHY port of Linux
  `sun6i_mipi_dsi`/`phy-sun6i-mipi-dphy` (`drivers/video/sunxi/sun6i_dsi.c`),
  and the ILI9488 init table (`drivers/video/ilitek_ili9488_dsi_lcd.c`).
  Selected by `CONFIG_VIDEO_LCD_PANEL_DSI_ILI9488`. Clocks: PLL3 297 MHz feeds
  the MIPI PLL (x1) and DSI sclk; TCON0 runs from the **MIPI PLL** (div 4);
  D-PHY 150 MHz from PLL6. Pixel clock ends up 12.375 MHz (61.8 Hz) instead
  of 12. Console rotation `CONFIG_VIDEO_LCD_ROTATION=3` is upright.
  Verified on hardware. Lessons from bring-up:
  - TCON0 must be clocked from PLL-MIPI. Fed from PLL3 at the same rate, LP
    DCS writes work (panel shows grey with vertical lines once on) but HS
    never starts: TCON0 block counter stays 0, DSI debug regs 0x2e0-0x2f4 0.
  - Upstream `sunxi_drc_init()` never opened the DRC/SAT AHB gates; fixed.
  - Linux order (LP init, then HS clock) works; stock's HS-clock-first order
    and its TRI2/safe-period/start-delay values are not needed.
  - DSI `CMD_CTL` TX flag (bit 9) is not a reliable "sent" indicator.
  - Debug method: dump stock registers via adb `/bin/io`; have a FEL-loaded
    U-Boot `cp.l` registers to SRAM A2 (0x44000, survives warm reset), set
    the RTC FEL flag and `reset`, then `sunxi-fel read 0x44000`.
    `tools/fel_dsi_test.py` drives DSI/DCS directly over FEL. FEL USB wedges
    after a few hundred `sunxi-fel` calls; power cycle to recover.
  To change the patch: edit `s30-build/u-boot`, then
  `git diff -- . ':!arch/arm/dts/Makefile' ':!configs' > patches/u-boot/0001-...patch`
  (new files need `git add -N` first).

## Stock firmware

SPI-NOR partitions (dump order):

| Part | Size | Content |
|---|---|---|
| p0 | 384 KiB | boot0 (eGON.BT0, plaintext) @0x0; Allwinner U-Boot 2011.09 package @0x6000 (scrambled) |
| p1 | 128 KiB | U-Boot env |
| p2 | 7.1 MiB | uImage "ARM Zkswe Linux-3.4.39" (scrambled), gzip zImage |
| p3 | 128 KiB | squashfs, nearly empty (`/dev/console`) |
| p4 | 256 KiB | "ZksweLogo" boot logo, 320x480 24-bit BMP, unknown compression |
| p5 | 0 | empty |

Scrambling:

- **Kernel (p2)**: first 131072 bytes (fex `[zkswe_attr] ksize`) XOR with
  4-byte key `35 6e a9 b7` (fex `kkey = -1213632971`). Undone by stock U-Boot.
- **U-Boot (p0 @ 0x6000)**: each 32-bit word from header offset 0x2c up to the
  header `length` field XOR `0x47000000` (byte 3 of each word ^ 0x47). Undone by boot0.

Boot flow: BROM -> boot0 -> U-Boot (`bootcmd = run setargs_nor boot_normal`,
`sunxi_flash read 40007fc0 boot; bootm`) -> kernel with built-in initramfs
(rootfs is the initramfs; no `root=`). Android-style `/init` + `init.rc`
starts `vold`, `MtpDaemon`, `daemon`; the game UI is `gmenu2x` launched from
the SD card (`/mnt/extsd/bin/gmenu2x`).

Decoded outputs in `firmware-decoded/`: `flash_decoded.bin`, `p0_boot0.bin`,
`p0_u-boot.bin`, `p1_env.txt`, `p2_kernel.uImage`, `p2_zImage`, `p2_Image`
(load 0xc0008000), `System.map` (recovered from kallsyms), `kernel.config`
(stock IKCONFIG), `p2_initramfs.cpio`, `ili9488_init.txt`, `p3_squashfs.img`,
`p4_zklogo.bin`.

## FEL mode (verified 2026-10-28)

No button combo: boot0 never reads keys, and stock U-Boot's `fel_key` check
needs a fex `[fel_key]` section (absent) and an LRADC key (all buttons are GPIO).
Working route: boot0 (`p0_boot0.bin` ~0x2374) jumps to BROM FEL (0xffff0020)
if RTC GP register `0x01f00108` == `0x5aa5a55a`, clearing it first (one-shot).
Stock firmware runs `adbd` as root (`persist.sys.zkdebug=1` on this unit) and
ships `/bin/io` (`/dev/mem`, `CONFIG_STRICT_DEVMEM` off):

```
adb shell '/bin/io -4 0x01f00108 0x5aa5a55a; /bin/reboot'
sudo sunxi-fel ver   # AWUSBFEX soc=00001667(A33), USB 1f3a:efe8
```

Only works with no bootable SD card inserted (the BROM tries SD first). The
flag only matters to the stock boot0: a bootable card never runs it.
Alternative: `fel-sdboot.sunxi` at 8 KiB on a card (cold FEL, see below).

## Dev boot loop (stock card, no flashing)

- `build.sh initramfs`: `rootfs.sqfs` (zstd squashfs, ~82 MB: Alpine
  minirootfs + `build/packages` + rootfs-overlay + retroarch.bin + launcher)
  and a ~1 MB loader `uInitrd` (busybox + musl + `build/s30-loader`). Also
  builds `fel.scr`.
- `build.sh fel` with the device on stock (adb up): `adb push --sync` kernel,
  DTB, uInitrd, rootfs.sqfs and logo.bmp to `/boot` on the card (~1 MB/s, only
  changed files), FEL flag, reboot, upload U-Boot + `fel.scr` (loads /boot from
  mmc 0:1). Without adb (device already in FEL, e.g. from Linux `s30-fel`)
  everything goes over FEL and the rootfs already on the card is used.
- Updating a running device over ssh (no reboot needed until you want the
  new files): copy to a temp name, then `mv` into place, `sync`:
  `scp s30-build/boot/zImage s30-build/uInitrd root@10.42.0.2:/mnt/sd/boot/`
  (as `*.new`, then mv). Never overwrite `rootfs.sqfs` while it is running:
  it is loop-mounted as `/`, and rewriting it in place breaks the running
  system. Instead copy it to `/mnt/sd/boot/rootfs.sqfs.new`: `s30-loader`
  renames it over `rootfs.sqfs` at the next boot, before mounting it.
- After boot `rc` clears `/dev/fb0` to black (after `s30-splash 100`), or
  fbcon shows the splash again between apps (when a KMS app exits).
- The old sunxi-tools (1.4.2) can't pass the script address for SPL header v3;
  `CONFIG_PREBOOT` sets `fel_scriptaddr` instead.
- USB under Linux is a configfs composite gadget set up by `/etc/s30/rc`:
  ACM serial shell (`/dev/ttyACM*`) plus NCM networking (verified
  2026-10-29). Device 10.42.0.2, udhcpd hands the host 10.42.0.1 (no
  router/DNS). `ssh root@10.42.0.2`, `scp`/`rsync` work (dropbear,
  sftp-server, rsync); scp ~6 MB/s (83 MB rootfs in 14 s). First boot
  generates the host key (takes a while).
- USB disk mode: hold SELECT at power on. `s30-loader` (initramfs) exports
  the whole card (`/dev/mmcblk0`) as a USB mass storage device before
  mounting anything, and shows `build/splash/usb.bmp` (regenerate with
  `tools/make_usb_splash.py`, kernel VGA 8x16 font). Ejecting on the PC or
  pressing START reboots (START without ejecting can lose unflushed host
  writes). Kernel + uInitrd copied to the card 2026-10-30, untested on
  hardware: unknown whether the gamepad (I2C) and the UDC are up in time
  (`s30-keyheld` waits 2 s, the loader 5 s for `/sys/class/udc`).
- Host side: NetworkManager profile `pocketgo-s30`, matched on the gadget's
  host MAC 02:53:33:30:00:01, DHCP, never-default, ignore DNS. Don't
  `nmcli dev connect` the USB interface without it: NM moves the unbound
  "Wired connection 1" profile off the main Ethernet and the host loses
  its internet. To recreate:
  `nmcli con add type ethernet con-name pocketgo-s30 ifname '*'
  ethernet.mac-address 02:53:33:30:00:01 ipv4.method auto
  ipv4.never-default yes ipv4.ignore-auto-dns yes ipv6.method disabled`
  `build.sh initramfs` bakes the build user's `~/.ssh/id_*.pub` into
  `/root/.ssh/authorized_keys` (override: `S30_SSH_KEYS="file ..."`); host
  key persists at `/mnt/sd/home/.dropbear/ed25519_host_key`. Serial is the
  fallback: don't push big files over it (a 7 MB base64 transfer wedged it).
- Root is squashfs + tmpfs overlay: changes outside the SD card are lost at
  reboot. For quick tests, edit `/etc/...` live and `killall s30-launcher`
  (init respawns the frontend).

## Linux system (BusyBox init)

- PID 1 is BusyBox init (`/etc/inittab`): sysinit `/etc/s30/rc`, then
  respawns the frontend (`/usr/libexec/s30/frontend`: s30-launcher, else
  RetroArch) and shells on ttyS0 and ttyGS0. `reboot`/`poweroff` work.
- `/etc/s30/rc`: mounts, udevd (eudev) + trigger (RetroArch's udev input
  needs `ID_INPUT_JOYSTICK`), mixer levels, USB gadget + udhcpd + dropbear,
  seeds `~/.config/retroarch/retroarch.cfg` from `/etc/retroarch.cfg` and
  missing `/mnt/sd/apps/*.sh` from `/usr/share/s30/apps`, joystick
  calibration, blue LED when done.
- `HOME=/mnt/sd/home`, `XDG_RUNTIME_DIR=/run/user/0` from
  `/etc/profile.d/s30.sh` (login shells, rc, frontend; `ssh host cmd` is not
  a login shell and keeps HOME=/root).
- System menu (s30-display, power key): Sleep / Reboot / Shut down / Cancel
  over the launcher (`reboot` / `poweroff` through init), Resume / Sleep /
  Exit game over an app. The PMIC node has
  `x-powers,system-power-controller`, so poweroff cuts power via the AXP.
- `s30-volumed` (utils/s30-volumed.c, respawned by init): volume keys
  (gpio-keys, polled, no autorepeat: the daemon repeats while held) step
  "Master Playback Volume" (0-63, step 3; drives speaker and headphones;
  the codec's headphone output volume, renamed in patches/linux/0001 since
  the speaker amp is fed from that output; apps' "Master" defaults find it)
  for every app; level saved to `~/.config/volume` 2 s after the last change
  and restored at boot. RetroArch's volume hotkeys are unbound (`nul`).
  Also watches the codec's jack input device (`...-jack`, SW_HEADPHONE_INSERT)
  and sends `osd ICON PERCENT` to s30-display on volume and jack changes.
- `s30-display` (utils/s30-display.c, respawned by init before the
  frontend): display broker + OSD, no GPU compositing. It finds the KMS card
  (card1; card0 is lima) and hands each app a DRM lease (connector, CRTC,
  primary plane) over `/run/s30-display.sock`; `libs30lease.so`
  (utils/s30-lease.c, LD_PRELOAD set by the frontend for the launcher and
  everything it starts) turns `open("/dev/dri/cardN")` into `lease PATH` and
  falls back to a plain open if the daemon refuses (not the KMS card, a lease
  already out) or is absent. The daemon is master only while a lease is out
  (a lessor must be master); the lessee goes when it closes the fd (polled
  with drmModeListLessees). The OSD is overlay plane 47 (topmost ARGB plane;
  the A33 backend has 3 overlays), a 220x40 box at the top drawn
  panel-rotated (landscape (x, y) -> panel (y, 479 - x)). The plane is
  enabled once per lease and stays on (transparent when idle): commits on
  the app's CRTC can make its page flip fail with EBUSY. Frames are drawn in
  RAM and memcpy'd into the scanned-out buffer right after a vblank (the
  daemon runs SCHED_FIFO 1 via the raw syscall: musl's
  sched_setscheduler() is an ENOSYS stub); drawing in place tore. Bar eases,
  box fades (~100 ms). It also shows external power plugged in/out
  (power_supply netlink uevents; the charger is `axp22x-ac`, not
  `axp20x-usb`) with the battery level, never "full" or a permanent battery.
  Restarting the daemon revokes the running app's lease (black screen):
  restart the launcher with it.
  System menu (power key, `axp20x-pek` grabbed): the plane is resized to the
  whole screen, transparent around a centred menu box and a battery box in
  the top right where the launcher has it (only one alpha plane can blend,
  so it reuses the OSD plane; the buffer is screen-sized, the OSD box uses
  its top-left 220x40 and only the shown part is copied in;
  the resize commits happen while the lessee is SIGSTOPped, so they can't
  collide with its flips; the plane is cleared and moved before the menu is
  drawn, else the old position flashes it). Lessee pid from SO_PEERCRED at
  lease time; gamepad grabbed while open. Row 0 of both menus is a
  brightness slider (sun icon + bar, D-pad left/right, held repeats; 32
  steps from 25 % of the range, below which the screen is all but off; the
  backlight and the bar ease to the new level per frame like the OSD bar,
  only the slider row copied in; saved to the file given as argument,
  /mnt/sd/home/.config/brightness from inittab, 2 s after the last change,
  restored at start; A on it does nothing). Then over an app: Resume / Sleep /
  Exit game (SIGTERM, SIGKILL after 2 s); over the launcher: Sleep / Reboot /
  Shut down / Cancel; the row B also triggers (Resume / Cancel) has a 14 px
  B button badge left of its text. The box is 160 px wide, 28 px rows of 1x 8x16 text
  (2x was too big), sized to its rows. Volume keys still work
  while it is open (s30-volumed's gpio-keys aren't grabbed); their OSD box
  is drawn above the menu, eased and faded like the normal OSD (only the
  box's area is copied in per frame). Battery box: see the launcher battery
  below; redrawn on power_supply uevents.
  Text: kernel VGA 8x16 font (utils/osd_font.h from tools/make_osd_font.py).
- Launcher text: the system menu's VGA 8x16 font (utils/osd_font.h, a glyph
  texture tinted per call; no bold (double-striking looked bad); non-ASCII bytes show
  as '?'); SDL2_ttf is no longer linked.
- Battery (launcher header and system menu, same look and place: right edge
  10 px in, centred 16 px down): 42x18 outlined battery filled to the level
  (green > 50 %, yellow > 20 %, red), the percentage inside in the 8x16 font
  with a shadow, a pixel bolt to its left while `status` is Charging (the
  battery doesn't move). Over a game the system menu shows it in a small box.
- `s30-fel` (utils/s30-fel.c): `reboot(RESTART2, "fel")`; the DT's
  nvmem-reboot-mode writes 0x5aa5a55a to RTC GP register 2 (an nvmem cell
  of the RTC), which boot0 checks once.
- Boot is quiet: cmdline `quiet vt.global_cursor_default=0
  fbcon=logo-pos:center loglevel=3` with `/dev/console` = ttyS0 (last
  `console=`): init, rc and kernel errors go to serial, not the panel; apps
  log to `/tmp/frontend.log`.
- GPU: lima + Mesa 26.1 (`mesa-dri-gallium`), kmscube runs on the panel
  (`kmscube -D /dev/dri/card1`; card0 is lima, card1 sun4i-drm), renderer
  "Mali400", GLES 2.0. Alpine's gallium pulls in llvm22-libs (~120 MB).

## RetroArch

- Own build (`build.sh retroarch`): v1.22.2 cross-compiled with clang + lld
  for armv7-alpine-linux-musleabihf against a sysroot of Alpine -dev packages
  (`s30-build/sysroot`); wrappers `s30-build/bin/armv7-clang[++]`. Drivers:
  gl (GLES2) on KMS/EGL, alsa, udev input. Alpine's retroarch package was
  replaced (sideways on KMS, no input without udevd, Qt bloat).
- Only `gl` makes sense on Mali-400: glcore/vulkan unsupported, sdl/fbdev are
  software only.
- Screen rotation (patch 0001): the panel is portrait (320x480), so gl2
  renders the whole frame (menu, shaders, screenshots, overlays) into an FBO
  at the logical 480x320 and draws it rotated to the real framebuffer before
  each swap. Driven by `screen_orientation` (3 is upright here; 1 is
  upside down), read at init only. The KMS context always gets the physical
  size. `video_rotation` is separate (rotates content only).
- Config highlights (`build/rootfs-overlay/etc/retroarch.cfg`): RGUI with
  3:2 aspect (`rgui_aspect_ratio = "7"` in the 1.22 enum, the old 1.9
  config's "5" is 21:9 now), lock fit, linear filter, full-width layout;
  `menu_swap_ok_cancel_buttons = "false"` (1.22 defaults to OK on B);
  L1+R1+Start+Select = menu (the power key is the system menu's), volume keys
  = system volume; clock off (no
  RTC battery); `config_save_on_exit = "true"`; dirs under
  `~/.config/retroarch`, ROM browser `~/roms`.
- Gamepad autoconfig: udev joypad numbers buttons in code order from
  BTN_JOYSTICK: SOUTH(B)=0 EAST(A)=1 NORTH(X)=2 WEST(Y)=3 TL 4 TR 5 TL2 6
  TR2 7 SELECT 8 START 9 THUMBL 10 DPAD 11-14; stick axes 0/1.
- Select-based hotkeys were tried and removed at the user's request.
- Cores from Alpine: fceumm, gambatte, gpsp, mgba.

## EmulationStation (started 2026-10-31)

- RetroPie's EmulationStation (submodule external/emulationstation), not
  ES-DE: ES-DE 2.0+ needs OpenGL ES 3.0, the Mali-400 has 2.0. Built by
  `build.sh emulationstation` (cmake, Mesa GLES 2.0 renderer
  `USE_MESA_GLES`, Alpine pugixml instead of the bundled submodule:
  upstream already uses a system pugixml when found; `_LARGEFILE64_SOURCE`
  because musl >= 1.2.4 hides stat64), installed under
  $WORK/emulationstation-root and copied into the rootfs.
- Patches (patches/emulationstation): 0001 from the AUR package (install
  rules, resources in /usr/share/EmulationStation); 0002 input setup without
  the right stick, right thumb and hotkey entries, and no hotkey prompt
  (games are exited from the power key's system menu).
- Runtime: freeimage, libcurl, pugixml, vlc-libs only (libvlc to link; no
  VLC plugins, so video previews don't play).
- Launcher app EmulationStation.sh: `--screenrotate 3 --screensize 480 320`
  (window is the panel's 320x480; the canvas is laid out landscape).
- /etc/emulationstation: es_systems.cfg (gb, gba, nes from ~/roms/<system>
  through RetroArch with gambatte, mgba, fceumm), es_input.cfg (the S30
  gamepad, copied to ~/.emulationstation by rc when missing), themes/carbon
  (submodule external/es-theme-carbon).
- Not yet verified: launching a game from ES (lease handover to RetroArch
  and back), how Carbon looks at 480x320.

## Power key, reboot (verified 2026-10-29)

- Holding power 6 s is a hardware power-off by the AXP (reg 0x36 = 0x5d:
  off enabled, 6 s). Bit 2 (0x04) is "restart after long-press power-off";
  stock sets it (fex `pmu_pekoff_func = 1`) and the AXP keeps it while the
  battery is connected, so the device turned off and straight back on.
  U-Boot patch 0004 clears it in SPL `axp_init()`. Verified live: with
  0x36 = 0x59 a long press stays off.
- Low battery wouldn't boot, even on the charger, until ~5 min of charging
  (2026-11-01): stock leaves Voff (reg 0x31) at 0x07 = 3.3 V, kept while the
  battery is connected, so the boot load dipped below it and the AXP cut
  power. Patch 0004 also sets Voff to 2.9 V (0x03, AXP default) in SPL
  `axp_init()`. Verified live: 0x31 = 0x03 after reboot. Not yet retested on
  an empty battery. Linux has no low-battery shutdown yet, so a running
  device is still hard-cut at Voff (no sync).
- Charging LED went dark (2026-11-01): the plug-in shutdown in SPL
  (`board.c`) wrote 0x80 to reg 0x32 instead of setting bit 7, zeroing
  bit 3 (CHGLED controlled by the charger) and bit 6 (battery monitoring);
  the AXP keeps that while the battery is connected. Patch 0004 now sets
  only bit 7 there and sets bits 6 and 3 in `axp_init()`. Verified: LED back
  on with 0x32 = 0x48; SPL restores 0x48 from 0x00.
- Plugging in USB while off powers the AXP on. U-Boot
  `CONFIG_AXP_DISABLE_BOOT_ON_POWERON` (defconfig): SPL checks reg 0x00 bit 0
  (power-on by plug-in) and powers back off, so only the power key boots. The
  battery still charges while off; no charging screen (a Linux charging mode
  in s30-loader was considered and rejected). Written to the SD card
  2026-10-30 (over ssh, `dd of=/dev/mmcblk0 bs=1k seek=8`), untested.
- In Linux the key is `axp20x-pek` (KEY_POWER), grabbed by s30-display for
  the system menu (RetroArch's menu: L1+R1+Start+Select). Clean power-off: the
  system menu over the launcher (Shut down = `poweroff`, AXP
  system-power-controller).
- Sleep (built 2026-10-31, untested on hardware): "Sleep" in the system menu
  (over apps and the launcher) clears the menu, syncs and writes `freeze` to
  /sys/power/state (s2idle; CONFIG_SUSPEND/PM_SLEEP in kernel.config). The
  app stays SIGSTOPped through sleep and resumes after wake. Wake = power key
  (axp20x-pek is a wakeup source, r_intc supports set_wake); on the AXP223
  the waking press still reaches userspace, so s30-display ignores power key
  presses for 500 ms after waking. sun4i-drm suspends via
  drm_mode_config_helper_suspend (panel off, state restored on resume). Long
  press = AXP hardware off as before. Apps must survive ALSA suspend
  (ESTRPIPE; RetroArch and SDL use snd_pcm_recover). Suspend-to-RAM would
  need Crust firmware, which likely doesn't support the A33.
- `reboot -f` (watchdog) warm-resets and boots the card again, with bit 2
  set or clear (RTC GP breadcrumbs survive). The old "reboot powers off"
  dated from the FEL dev loop, when the card had no U-Boot.
- Write AXP registers from Linux without regmap debugfs writes: drive the
  RSB controller directly with devmem2 (CMD 0x2c = 0x4e write8, DAR 0x30 =
  0x2d0000, ADDR 0x10, DATA 0x1c, CTRL 0x00 = 0x82). Races the kernel
  driver; retry and check with `cache_bypass` + `registers`.

## Audio, jack, LEDs (verified 2026-10-28)

- Speaker: mono amp enabled by PH9 (simple-audio-amplifier), bridged across
  the codec's HPL/HPR (stock: left HP PA unmuted, right carries inverted left
  via PAEN_HP_CTRL bit 1 "LTRNMUTE", HPCOM on). Mixer defaults that stock
  uses: AIF1 DA0 160, DAC 160, AIF1 slot 0 DAC switch on (set by rc).
- Jack: the codec driver owns it (`hp-det-gpios` on codec-analog, handed a
  jack by simple-audio-card through set_jack). On each change it sets
  controls: no headphones = DAC route Mix Mono, "Headphone Mono
  Differential" on, right HP muted, Speaker pin on; headphones = Reverse
  Stereo (`allwinner,headphone-lr-swapped`: the jack has L/R swapped;
  checked with 3-pole headphones, a TRRS headset mixes channels oddly),
  differential off, Speaker pin off. Instant, no userspace.
- Jack detect PL5 (low = plugged) only reads correctly while the headphone
  amp is powered: patch 0001 adds `allwinner,headphone-amp-always-on`. The
  amp is enabled once at probe outside DAPM (widget is SND_SOC_NOPM, event
  skips). It used to be `snd_soc_dapm_force_enable_pin()`, but a forced,
  already-on widget never propagates power changes, so the speaker path
  stayed off when a stream started (needed a "Speaker Switch" toggle).
- Plugging a headphone reset the analog codec to its power-on defaults
  (HP volume 0, HPPAEN off; reg 0x07 = 0x14) and the jack then read
  "plugged" forever: silence until a cold boot. Cause: HPCOM (the capless
  headphone return) was a DAPM supply, off while idle, so the always-on amp's
  DC bias loaded through the plug. Patch 0001 now keeps HPPAEN + HPCOM_FC +
  COMPTEN on together (reg 0x07 = 0xfc idle; "Headphone Amp Playback
  Switch" switches all four) and re-asserts them on every jack change.
- Boot/shutdown pops (much quieter now): the amp comes up with the longest
  PA anti-pop ramp (reg 0x07 bits 3:2 = 11) and HPCOM 50 ms before HPPAEN;
  the platform `.shutdown` hook mutes, turns the PA off, waits 600 ms for the
  ramp, then drops HPCOM before poweroff/reboot cuts the supply.
- Blue LED: PL10, active low (`gpio-leds`, function status).
- Red LED: AXP CHGLED (2026-11-01). Reg 0x32 bit 3 = charger drives it (red
  while charging); with bit 3 clear, bits 5:4 = 00 float it (LED off).
- Green LED: shows on stock at some point, driver not found. Stock fex has no
  LED pins ([leds_para] absent); the stock kernel has `pmu_chgled_func/type`
  and `arisc_set_led_bln` (LED via the AR100/CPUS). Tested without effect
  (charger in): CHGLED floating, AXP GPIO0/GPIO1 LDO on and drive low
  (U-Boot `axp_init()` sets both to input), PL2/PL3/PL4/PL7 high and low,
  earlier PL6, PE17.
- Stock `tinyplay`/Alpine tinyalsa 2.0 `tinyplay` play nothing; use `aplay`.

## Boot splash

- Linux: `s30-splash PERCENT` (utils/s30-splash.c) draws the same
  `logo.bmp` (installed as /usr/share/s30/splash.bmp) on /dev/fb0
  (`CONFIG_FB_DEVICE`) with a purple progress bar under the logo (panel x
  222-228, y 140-340: below the logo as held, filling left to right);
  /etc/s30/rc calls it at 5/40/60/75/100 %. The launcher then takes the
  display over.
- U-Boot -> kernel handoff: the panel driver's `handoff` parameter
  (`panel_ilitek_ili9488_dsi.handoff=1`, set by fel.cmd/boot.scr since
  U-Boot always initializes the panel for its splash) makes the first
  prepare skip the reset + init sequence: the blank went from ~450 ms to a
  ~60 ms blink. DSI reads return 0 on this host, so the panel state can't be
  detected. The remaining blink is sun4i-drm resetting the TCON and clearing
  the backend layers at bind; removing it would need sun4i to adopt
  U-Boot's running state (not done).

- Stock logo: SPI p4 "ZksweLogo", LZO1X (sizes at 0x14/0x18, data at 0x40)
  -> 320x480 24-bit BMP in panel-native orientation (`tools/zklogo.py`).
  `build/splash/logo.bmp` is that with near-black noise cleaned to black.
- U-Boot: `SPLASH_SCREEN` + `SPLASH_SOURCE`, env from patch 0002
  (`splashimage` = PXE slot 0x43200000, `splashsource=mmc_fs`,
  `splashfile=/boot/logo.bmp`, stdout/stderr serial only so the LCD shows
  only the logo), `HIDE_LOGO_VERSION`, `BMP_24BPP`.
- Linux: `CONFIG_LOGO_LINUX_CLUT224_FILE` = the logo rotated upright and
  cropped to 286x210+97+55 (symmetric about the screen centre), so
  `fbcon=logo-pos:center` puts it exactly where U-Boot drew it.

## Standalone SD card boot (verified 2026-10-28, FAT32)

`sudo build.sh sdcard /dev/sdX [format]` makes a card that boots on its own:
the A33 BROM tries the card before SPI, so U-Boot at 8 KiB runs and distro
boot runs `/boot/boot.scr`. Without `format` it keeps the existing filesystem
(exFAT, FAT or ext4) and only refreshes U-Boot and /boot; `format` ERASES the
card and makes one exFAT partition at 1 MiB. Refuses non-removable disks and
partitions starting before 1 MiB. Needs a terminal for sudo (the `!` shell in
Claude Code has no tty; run it in a normal terminal).

Card layout:

```
/boot/  zImage  sun8i-a33-pocketgo-s30.dtb  uInitrd  rootfs.sqfs  logo.bmp  boot.scr
/home/  ($HOME) roms/  .config/retroarch/{retroarch.cfg,saves,states,system,screenshots}
```

- U-Boot's fs/exfat was broken on 32-bit ARM (patch 0003): it did not build
  bare-metal (`#error Unknown platform`, missing `__aeabi_ldivmod`), and the
  device size (32-bit sector count x block size, `fs/exfat/io.c`) overflowed,
  so any card > 4 GiB looked smaller than its filesystem and mount failed
  (`fstype` fails). 64-bit sandbox does not show it (`ulong` is 64 bits).
  Fix built, not yet tested on hardware.
- A card U-Boot can't read looks dead: no splash (stdout is serial only), no
  kernel, black screen, no LEDs (LEDs stay off until Linux drives them). The
  first ext4 attempt failed the same way for another reason (the loader only
  mounted vfat; it now autodetects).
- Ruled out while debugging: DRAM (2 MB write/readback at 4 spots incl. top
  of RAM from cold: 0 errors), PMIC (cold vs stock-boot0 after SPL only differ
  in Voff 0x31, Vhold 0x30 and ADC readings; battery 4.19 V), display, SPL.

Debugging without serial:

- `s30-build/fel-sdboot.sunxi` (8 KiB, from kg9316/fel-mass-storage's
  fel-sdboot.img at offset 8 KiB; don't write the whole 2 MB image, it has
  its own partition table) at card offset 8 KiB gives a cold FEL entry (no
  stock boot0 ran): `sudo dd if=s30-build/fel-sdboot.sunxi of=/dev/sdX bs=1k
  seek=8 conv=notrunc,fsync`. Rerun `build.sh sdcard` to restore U-Boot.
- RTC GP registers (0x01f00100..0x01f0011c, avoid 0x108 = boot0 FEL flag)
  survive a forced power-off: write breadcrumbs from a U-Boot script
  (`mw.l 0x01f0010c ...`) or C (`writel`), force off (hold power 10 s), read
  them on the next FEL entry with `sunxi-fel readl`.
- After U-Boot hangs, the FEL USB device stays listed but dead
  (`sunxi-fel ver` hangs): force power off.
- `tools/fel_axp_dump.sh`: AXP223 registers over RSB from FEL (after SPL).
- A U-Boot USB ACM console (`USB_FUNCTION_ACM`, stdout usbacm in preboot)
  never enumerated here; unverified whether it works on this board.

## Build

Run on Arch or Debian/Ubuntu (`deps` uses pacman or apt). Toolchain prefix is
`$CROSS_COMPILE` if set, else `arm-linux-gnueabihf-` or `arm-none-eabi-`.

```
bash build/build.sh deps
for s in linux uboot rootfs retroarch initramfs; do bash build/build.sh $s; done
bash build/build.sh fel                                      # dev loop
sudo bash build/build.sh sdcard /dev/sdX [format]            # standalone card
```

Upstream versions are the submodule commits (external/). To move one: check
out the new tag in external/<name>, commit the submodule, delete
$WORK/<name> (it is a worktree with the board patches applied) and rebuild.

The old `image` step (ext4 `out/sdcard.img`) predates the squashfs + /boot
layout and does not match it; use `sdcard`.

## Display notes

- Linux display works: full-screen console via DRM fbcon, panel `rotation`
  270 (upright). Three fixes were needed:
  - DTS: `&tcon0` `assigned-clock-parents = <&ccu CLK_PLL_MIPI>` (as U-Boot).
  - Panel mode clock 12375 kHz, not 12000: for 288 MHz the clk framework set
    PLL-MIPI to `0x90c00d1d` (N=14 K=2 M=14) and the panel showed rows
    squeezed into 2/3 of the screen. 12375 = 297 MHz / 24 gives PLL-MIPI x1
    (`0x90c00000`), as stock firmware and U-Boot use.
  - U-Boot must not hand over a running DSI block: Linux only releases the
    shared DSI/D-PHY reset, and its first LP write (`e0 ...`) then times out
    (-110). `sunxi_de` now has a DM_FLAG_OS_PREPARE remove hook that puts
    DSI back in reset before bootz.
- Debug method: shell over USB serial, `devmem2` to read and poke registers
  live. DSI registers read 0 unless the DSI bus gate (CCM 0x060 bit 1) is
  opened first; Linux gates it between accesses.
- Known issue: WARN at boot from `drm_connector_set_panel_orientation()` in
  the panel's get_modes (property created after DRM registration). Cosmetic.
- Backlight PWM is active high (verified). The DT has no brightness-levels
  (was 8 hand-picked levels): pwm-backlight's own CIE 1931 table, 1334
  levels for the 20000 ns period, default the middle one.

## Ocarina of Time port (ports/oot, submodule) (started 2026-10-29)

Repo: github.com/bkacjios/oot-s30 (submodule `ports/oot`, branch main),
started from github.com/jnmartin84/oot-dc (history not kept).
All Dreamcast code is stripped; the tree is S30-only. ROM: `gc-us` (USA
GameCube, regular quest) at `baseroms/gc-us/baserom.z64` (git-ignored), from
`/run/media/jake/games/Roms/N64/Legend of Zelda, The - Ocarina of Time (USA) (GC).z64`.

Build (from ports/oot):

```
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt   # once
make -f Makefile.s30 extract    # ROM -> extracted/gc-us, build/gc-us (12 s)
make -f Makefile.s30 assets     # ARM asset segments -> data/*.bin, build_assets/assets (1.5 min)
make -f Makefile.s30 -j$(nproc) # zelda
rsync -rt zelda data root@10.42.0.2:/mnt/sd/home/ports/oot/
```

- Game code: arm-none-eabi-gcc (needs `-fno-toplevel-reorder`) against the
  Alpine musl headers, `-fno-short-enums`; `src/s30/` (EGL/DRM/ALSA) and the
  link use the clang wrapper. Asset symbols come from
  `build_assets/assets/symbols.ld` (lld rejects the duplicate absolute symbols
  of `--just-symbols` on every asset ELF).
- `src/s30/`: display.c (KMS/GBM/EGL, 427x320 offscreen FBO rotated onto the
  portrait panel, 3 vblanks per frame = 20 fps; orientation verified),
  gfx.c (renderer; still a test pattern), audio.c (ALSA 32 kHz, audio thread
  paced by blocking writes; music verified), input.c (user layout: Y=B, B=A,
  X=C-left, A=C-down, R1=C-right, L1=L, L2=Z, R2=R, Select=C-up, Start=Start,
  D-pad = N64 D-pad for future D-pad item slots; L1+R1+Select+Start quits),
  main.c (`OOT_DATA`, `OOT_SAVE` default `$XDG_DATA_HOME/oot/oot.sav` = `~/.local/share/oot/oot.sav`; an old `~/oot.sav` is moved there, `OOT_NOAUDIO`).
- Kept from the DC port (renderer depends on it): float `Mtx`
  (guMtxF2L = memcpy), texture-cache hooks, pause cursor vertices built at
  draw time. PVR workarounds removed 2026-10-30 (src/dreamcast deleted, DC
  DL markers gone; "BLND" w0 now carries only S30_MARK_* ops): VisMono is a
  GPU pass (S30_MARK_VISMONO, prim/env from the RDP state); sun lens flare
  and light glows depth-test the last frame's z-buffer again, via
  S30Gfx_DepthVisible from the graph callback (called before each frame's
  DL walk). Probes are drawn at the end of the frame (1-pixel scissored
  depth-test quads, gathered into a 32x1 strip) and read at the next
  frame's start, so results lag one frame: a synchronous glReadPixels per
  probe cost 2.3-21 ms/frame on lima, the async strip read 0.1 ms; vanilla
  gSunDL/moon, freezard steam and sun position restored. N64 boot logo is
  skippable with Start/A/B (the PRESS START buttons): fast fade-out.
- DC bugs fixed for ARM: audio skipped notes whose pointer < 0x7FFFFFFF
  (valid on DC only); per-note synthesis buffers never allocated; IO_READ of
  N64 registers (segfault) -> 0 on LINUX; osGetCount 1000x slow; DMA buffers
  sized for MQ debug files (now per-segment RAM cache); libultra message
  queues unlocked across threads (`__osDisableInt` = recursive mutex);
  software mixer (deleted in DC history, restored) de-SH4'd.
- Audio/samplebank/soundfont tables now derived from the ROM by
  `pc_tools/dc_meta.py` (was a static MQ-debug JSON); sequence font table in
  reimpl.c verified byte-identical to gc-us.
- Testing on device: `mount --bind /bin/true /usr/bin/retroarch; killall
  retroarch` frees the display (init's frontend then runs `true`); `umount`
  restores it. gdb: `sh /mnt/sd/tools/gdb.sh` (Alpine gdb + deps on the card).
- Renderer (src/s30/gfx.c, 2026-10-29): own GLES2 F3DEX2/RDP interpreter
  (DC PowerVR one dropped). CPU vertex transform/lighting/fog, TMEM emulated
  in N64 byte order (RAM textures are LE u64: byte A at A^7), content-hashed
  texture cache, generated combiner/blender shaders, 3D at 427x320, 2D rects
  1:1 in a 320x240 layer composited with sharp bilinear. Title screen,
  logo, field, sky, moon verified by the user. Lessons: G_CYC_/G_TF_/G_TT_
  constants are pre-shifted; N64 front faces are CCW in GL; mask 0 = no
  wrap (clamp); no half-texel offset; fog uses winv=32767 for w<0; NTSC
  (gc-us) loads the title font from big-endian jpn message data (bswap).
  GL state cache (gfx.c sGL): gl_cache_reset() must poison every field; a
  single "valid" flag went stale and kept composite_2d's blend func, so
  blended 2D draws (N64 logo text box) came out opaque. Lima shader
  compiler bug: separate clamp() on the rgb and alpha of the output makes
  a texture alpha that feeds only alpha read as 0 (file-select highlight
  invisible); the generator clamps each cycle as one vec4 instead.
  Second lima bug: `if (ca < max(uBlend.a, 1.0 / 255.0)) discard;` discards
  every fragment (any max() in that compare); the alpha-compare threshold is
  now clamped on the CPU and the shader compares against uBlend.a alone.
  Symptom was the last (alpha 255, G_RM_OPA_SURF + G_AC_THRESHOLD) fade
  frame drawing no cover (N64 logo flashed at full brightness while the
  title loaded). Offscreen GL tests can run over ssh on /dev/dri/renderD128
  (GBM + surfaceless EGL) without stopping the launcher.
  Prerendered rooms: S2DEX G_BG_COPY/G_BG_1CYC (handle_bg) upload the image
  Room_DecodeJpeg decoded in place (native-endian RGBA5551, not the ^7
  layout) as one texture and draw it as a 2D-layer rect.
  libultra timers: osRecvMesg never blocks on Linux and nothing services
  __osTimerList, so Sleep_Cycles (libc64/sleep.c) uses nanosleep; the
  timer+recv version left a dangling stack OSTimer and crashed on the
  second pause-menu open.
  Pause menu (2026-10-30): the DC port redrew the frozen world (no actors,
  so Link vanished; its XLU list landed over the menu) and drew pause Link
  inline in the page with the world's depth. Now N64-faithful: the
  background is a GPU copy of the frame (DL markers S30_MARK_BG_SAVE/RESTORE,
  s30_platform.h -> S30Display_Save/RestoreBackground), and RGBA16 color
  images other than the 320-wide frame are "aux" render targets in gfx.c
  (own FBO + depth, 1:1 N64 pixels), copied back to RAM as RGBA5551 (^7
  byte order) when a texture load reads them. Pause Link uses the original
  64x112 offscreen render + PreRender copy (coverage/AA filter skipped).
  This needs real framebuffer RAM: SysCfb_Init on LINUX now points the two
  framebuffers at static arrays. The old LINUX SysCfb_Init was a GCC nested
  function inside the N64 one (never called), so the N64 body ran and set
  the framebuffers to unmapped 0x803B5000/0x803DA800; the readback
  segfaulted on opening the pause menu. Check for nested functions with
  `nm zelda | grep -E ' [tT] \w+\.[0-9]+$'`. Crashes print
  "crash: signal ... pc ... stack ..." to stderr (/tmp/frontend.log via the
  launcher); resolve with llvm-addr2line -f -e zelda.
  Frame pacing follows the game's R_UPDATE_RATE (S30Display_SetUpdateRate
  from Graph_TaskSet00): 3 vblanks (20 fps) in play, 2 in the pause menu,
  1 on the N64 logo/title/opening; a fixed 3 ran those at 1/3 and 2/3 speed.
  Debug env from the launcher: /mnt/sd/home/ports/oot/oot.env (VAR=value lines, sourced
  by "Ocarina of Time.sh"), e.g. OOT_GFXSTATS=1 (stats every 100 frames in
  /tmp/frontend.log; adds a glFinish per frame).
  Debug env: OOT_FAST (no 20 fps cap), OOT_SHOT=N (/tmp/oot_shot.ppm),
  OOT_GFXTRACE=frame, OOT_GFXSTATS, OOT_3POINT (N64 3-point filter in shader,
  costs ~3x GPU), OOT_DBG_ALPHA/NOBLEND, OOT_AUDIODBG (log each audio sample
  the first time it plays). Device test: run-test.sh SECONDS.
- Audio fixes: `AUDIO_RELOCATED_ADDRESS_START` was K0BASE (0x80000000), but
  this port's pointers sit at 0x40000000+, so every relocated font pointer
  looked unrelocated: `Audio_GetSoundEffect`/`Audio_GetDrum` returned NULL
  (no Link voices, other sfx and drums muted); now 0x10000000 on LINUX
  (include/audio.h). The DC mixer's `aHiLoGainImpl` (per-note gain) treated
  samples as unsigned, clamped before the >>4 and packed the wrong halves:
  boosted sfx (wall bump) came out quiet and crunchy; replaced with the RSP
  behaviour. The note sample-tracking arrays in load.c were raised past 256
  (font 0 needs ~490 book/loop entries). The comb filter (synthesis.c) is
  still disabled from the DC port.
- Performance (paused at user's request): GPU ~6 ms/frame with hardware
  bilinear; CPU display-list walk 13-21 ms, of which glDrawArrays ~4-7 ms and
  other flush work ~3-5 ms for ~80-210 draws (lima ~70 us/draw); worst
  frames ~40 ms vs the 48 ms 20 fps budget, so heavy scenes (Epona dust)
  drop frames. Ideas: fewer draws (texture changes force them), cheaper
  per-draw state, offload walk/GL to another core. mesa_glthread had no effect.
- Renderer performance, OoT and MM (src/s30, kept identical apart from env
  var prefixes; 2026-11-01). Busy Clock Town went from ~12 to ~20 fps:
  - Flips are queued, not waited for (wait_flip at the next EndFrame): the
    next frame's game logic and walk overlap the GPU.
  - Vertices stream through a ring of 3 VBOs written with unsynchronized
    glMapBufferRangeEXT (client arrays cost lima ~40% more per draw;
    glBufferSubData on a buffer the pending job reads flushes the job, ~1 s
    a frame in a benchmark; orphaning a 4 MiB buffer per frame had the
    kernel zero it every frame).
  - Depth probes (sun/lens-flare visibility) are read from the frame before
    last (two gather targets): reading the last frame stalled until the GPU
    finished it, which showed as 12-15 ms of "game" time.
  - Texrects sharing tile and state batch (sBatchTile); load memos use a
    generation instead of an 8 KiB memset per load; TMEM copies swap u64
    words; texture hash in 32-bit lanes; face culling per triangle on the
    CPU (det(x,y,w), so cull changes don't end a batch); 1-cycle draws mask
    the second combiner cycle.
  - What's left is mostly lima per draw (~45% of CPU incl. ioctl):
    ~320 draws/frame in busy Clock Town, most ended by texture loads and
    combiner changes (counts per cause in the stats line).
  - Debug env: <GAME>_GFXSTATS=1 (stats + late frames + draws by cause),
    =2 adds per-draw timers and a glFinish (GPU time, no overlap);
    <GAME>_PROFILE=1 samples pcs to /tmp/<game>_prof.txt every 500 frames
    (symbolize the binary with addr2line, libraries from the map lines);
    <GAME>_NOVBO=1 back to client arrays.
- clock_gettime was a 730 ns system call: Linux used the secure physical
  arch timer, so the vDSO had no clock. U-Boot now boots non-secure without
  HYP (CONFIG_ARMV7_VIRT off) and the board DTS deletes
  arm,cpu-registers-not-fw-configured (U-Boot sets CNTFRQ and a zero
  CNTVOFF on every core): virtual timer, 149 ns, monotonic across cores.
- Near-plane depth: NoN draws what is in front of the near plane (the RDP
  clamps its depth); the port clamped per vertex, which pulled whole ground
  triangles crossing the plane in front of everything (from exact camera
  spots). Now triangles crossing it are split at the plane (tri()), and
  depth has a near slice, NDC [-1, -1 + 1/64), ordered by -w/z (depth_z):
  a clamp to one depth instead made the slingshot and the hand fight. The
  far range is mapped linearly into the rest; probes use depth_ndc.
- Sun: gSunDL draws a 64x64 i4 texture as three I8 strips (two half-width
  suns side by side in each); replaced by one i4 quad (sSunDL in
  z_kankyo.c, as 2Ship2Harkinian does).
- Open: "DMA FAIL"
  for ovl_Demo_Du/Ec, Boss_Mo/Va .bin at boot (overlays are linked in; check);
  one-off "(null)/Audiotable.bin" DMA path and a drmModePageFlip failure seen
  once; D-pad item slots: reference `ports/Shipwright` (search DpadEquips).

## Super Mario 64 port (ports/sm64ex, branch s30) (2026-10-29)

- Fork of fcorbier/sm64ex (made for the stock firmware: vendor ZK toolchain,
  stock SDL with keyboard-key buttons). Branch `s30` adapts it to this
  system: `TARGET_S30` builds with the clang wrapper/sysroot via `S30_CC`/
  `S30_CXX` (host tools keep native cc), `-DTARGET_S30`, `-lGLESv2 -lSDL2`;
  clang strictness fixes (missing prototypes, goddard cast).
- Build: `make -j$(nproc) S30_CC=$PWD/../../s30-build/bin/armv7-clang
  S30_CXX=$PWD/../../s30-build/bin/armv7-clang++` with `baserom.us.z64`
  (Super Mario 64 (USA), sha1 9bef1128...) in the repo; output
  `build/us_pc/sm64.us.f3dex2e`, stripped to `/mnt/sd/home/ports/sm64/sm64`. Launcher
  entry "Super Mario 64.sh" (default in /usr/share/s30/apps).
- Saves/config: SDL pref path `~/.local/share/sm64pc` (the fork had
  hardcoded /mnt/extsd paths).
- Input: SDL game controller (auto-mapped evdev pad), upstream
  controller_sdl read restored. Defaults = the fork's original S30 layout:
  A and B -> N64 A, X and Y -> N64 B, L1/R1 -> L/R, L2/R2 -> Z, D-pad -> C
  buttons, Start; rebindable in Options. L1+R1+Select+Start quits. Deadzone
  back to 16 (fork used 28 for the stock firmware's noisy stick).
- The fork's rotation for the portrait panel (gfx_pc.c) is kept; runs on
  kmsdrm (window 320x480). The user played it; no rendering issues reported
  yet. Exits cleanly on SIGTERM (saves config).

## Shared N64 library (n64/) (2026-11-03)

- OoT, MM and SF64 link `n64/` (`n64.mk`) instead of per-port copies of
  src/s30/{display,gfx,input,audio}.c and the mixer. Started from MM's
  copies (the newest: replacement textures, motion blur, audio lock,
  overlap-safe interleave). Each game: `S30Gfx_Configure` (ucode, segment
  table, float or s15.16 Mtx, texture element sizes, env prefix), a button
  map (src/s30/buttons.c), its audio update (`S30Audio_Init`), its own Acmd
  macro header over `s30_mixer.h`.
- F3DEX (SF64) is translated per command to F3DEX2 (`f3dex_to_f3dex2`):
  opcodes, matrix flags (F3DEX2 inverts push), geometry bits (cull,
  smooth), movemem/moveword indices, NUML and light colour offsets,
  othermode shift/len.
- Texture addresses below 16 MiB are rejected as unresolved (was below
  0x10000000: SF64's segments live at 0x01000000-0x0FFFFFFF).
- n64.mk must not `-include` its .d files (they became the default goal);
  the port includes them after its first target.
- Verified on the device after the move: SF64 (title, 3D, audio), OoT
  (runs), MM (Clock Town, map overhaul, D-pad items).

## Star Fox 64 port (ports/sf64, submodule) (2026-11-03)

- Fork of sonicdcer/sf64 (bkacjios/sf64-s30, master); sf64-dc used only as a
  reference. Build: README.md there (`make -f Makefile.s30 extract assets`,
  then the build); Torch from sf64-dc's build (same commit; Torch's own
  cmake fails on newer CMake). Card: /mnt/sd/home/ports/sf64, launcher
  "Star Fox 64.sh", test: tools/s30/run-test.sh SECONDS.
- Asset segments are mapped at their segmented addresses (0x0N000000; the
  game's SEGMENTED_TO_VIRTUAL and K0/physical macros are identities);
  overlays linked in, .data/.bss in ovl_<name>_* sections reset on load.
- Pitfalls found: F3DEX negative opcodes in a u8 switch; libultra
  sinf/cosf (big-endian doubles) gave 0; Mtx halves (Matrix_ToMtx writes
  u.i/u.f, guMtxF2L packed words); texture byte order per array type
  (per-segment table, segments share addresses); audio: AudioCmd views,
  0x7FFFFFFF note-pointer check, relocation threshold, BE sound fonts.
  All in n64/README.md's checklist.

## Mario Kart 64 port (ports/mk64, submodule) (2026-11-04)

- Fork of n64decomp/mk64 (bkacjios/mk64-s30, master). Build: README.md
  there (`make -f Makefile.s30 extract assets`, then the build). Card:
  /mnt/sd/home/ports/mk64 (`mk64`, `data/mk64.dat`), launcher "Mario Kart
  64.sh" (tools/s30/mario-kart-64.sh). Played through menus and races on the
  device; ghosts aren't saved (no Controller Pak).
- Data: one file, mk64.dat, with the ROM's data segments at the game's ROM
  addresses (tools/s30/make_rom.py). Texture/sound data assembled big-endian
  (ROM bytes); C data (course display lists, vertices, paths, common/menu
  data) compiled little-endian, MIO0-compressed where the N64's is. `--verify`
  builds everything big-endian and finds it in the ROM: the native layout is
  the N64's, which the game's hard-coded segment offsets need.
- Texture byte order: RAM is N64 order (elem 1) except the native segments'
  u16/u32 arrays, registered where each segment is loaded and dropped when
  DMA or a decoder overwrites that memory (not on allocation: the game
  allocates over the startup logo while drawing it). CPU code that writes
  pixels (menu recolouring, the red shell palette) writes N64 order.
- Segment 0 is the image base (0x40000000) and SEGMENT_NUMBER2 keeps 4 bits:
  the game reaches its own RAM as offset + segment 0 (KSEG0 on the N64; it
  indexed past the 16-entry table and got 0 by luck).
- Audio: 26800 Hz (ALSA plug, hardware side pinned to 32 kHz) and clock-paced
  updates (S30Audio_Configure) so the game's AI-length buffer sizing works;
  256-frame periods (the codec reports position per period). The microcode is
  Mario Kart's (mupen64plus-rsp-hle alist_process_nead_mk is the reference):
  envelope mixer (wet from the panned dry, flags invert), no reverb ramp,
  8-byte DMA rounding, byte-wise DMEM moves.
- Renderer additions: F3DEX 0.95 (G_QUAD, old G_CULLDL), the default lookat
  (reflective logo), copy-mode alpha compare and coverage-times-alpha discard
  (HUD cutouts, item sprites), S30Gfx_FrameCopy (the jumbotron copies the
  last frame on the GPU).
- Debug env (mk64.env): MK64_GFXSTATS, MK64_GFXTRACE=frame, MK64_AUDIODBG
  (queue, underruns, notes by kind), MK64_AUDIODUMP (/tmp/mk64_audio.raw),
  MK64_AUDIOMASK. Crashes: crash.log beside the binary.
- Open: the jumbotron in split screen assumes the 1P widescreen layout.

## Panel refresh modes (2026-11-03)

- The panel driver lists 59.99 Hz (preferred, vtotal 540), 61.82 Hz (stock
  timing, 524), 59.77 Hz (GBA, 542) and 49.99 Hz (PAL, 648): same 12.375
  MHz clock and lines, vertical front porch changed. All measured on the
  panel. N64 ports now run exactly 20 fps (was 20.6, 3% fast).
- Apps pick one by rate (`S30Display_SetRefresh`; `<PREFIX>_REFRESH`
  overrides, `<PREFIX>_REFRESHTEST=1` prints the measured rate). U-Boot
  keeps 524 lines (the kernel reprograms TCON at bind).
- FPS overlay (s30-display): a frame stamped in the same millisecond as
  the overlay's clock wrapped `now - t` and showed "-"; fixed (frame_age).
- Card backups in /mnt/sd/boot: zImage.pre60 (61.8 Hz), zImage.60only,
  rootfs.sqfs.pre-fps, uInitrd.pre-fps.

## Next session

- Test Sleep on hardware: panel off/on, wake on power key, audio after
  resume, current draw while asleep (s2idle saves less than suspend-to-RAM).
- Verify Shut down from the launcher actually cuts power.
- `s30-fel` (reboot into FEL via nvmem-reboot-mode) is untested.
- I2C errors to the gamepad MCU (addr 0x60) every ~5.6 s in dmesg; harmless
  so far, not investigated.
- Kernel warnings at boot: the panel creates its orientation property in
  get_modes after the connector is registered (drm_mode_object_add WARN);
  fix by creating it before registration (sun6i-dsi bind has no panel yet).
- OoT: comb filter still disabled; D-pad item slots; performance (paused).
- Shared library next: libultra stand-ins (OoT/MM reimpl.c vs SF64 ultra.c)
  and the asset builders (pc_tools/make_assets.py diverged per port).
- TMC (SDL3) and RetroArch could pick the 59.77 / 50 Hz modes.
- Remaining U-Boot -> kernel blink (~60 ms): sun4i-drm resets TCON and
  clears the backend at bind (accepted as is).
