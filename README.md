# PocketGo S30 custom firmware

Mainline Linux, U-Boot and a lean Alpine userland with RetroArch for the
PocketGo S30 (Allwinner A33). It boots from the SD card and leaves the stock
firmware in SPI flash untouched: remove the card to get the stock system back.

No ROMs, BIOS files or stock firmware are included.

## Build

Needs an Arch or Debian/Ubuntu host. The pinned upstream sources (Linux,
U-Boot, RetroArch) are submodules; fetch them without history (`build.sh`
also does this on first use):

```
git clone https://github.com/bkacjios/pocketgo-s30-cfw
cd pocketgo-s30-cfw
git submodule update --init --depth 1
bash build/build.sh deps            # host packages (pacman or apt)
for s in linux uboot rootfs retroarch initramfs; do bash build/build.sh $s; done
sudo bash build/build.sh sdcard /dev/sdX          # refresh U-Boot and /boot
sudo bash build/build.sh sdcard /dev/sdX format   # ERASES the card first (exFAT)
```

Build trees and outputs go to `s30-build/` (override with `WORK=`). The
cross toolchain is `$CROSS_COMPILE` if set, else `arm-linux-gnueabihf-` or
`arm-none-eabi-`; userland is built with clang against an Alpine sysroot that
`build.sh` downloads.

ROMs go in `/home/roms` on the card.

## Layout

```
build/            build.sh, kernel config, U-Boot board files and patches,
                  initramfs loader, rootfs overlay (init, launcher apps,
                  RetroArch config), boot scripts, splash images
linux/            Board devicetree, panel and gamepad drivers, kernel patches
launcher/         s30-launcher (file browser) and joystick calibration
utils/            Small system tools (volume daemon, splash, FEL reboot, ...)
retroarch/        RetroArch patches
tools/            Firmware decoding and host-side helpers
external/         Submodules: linux, u-boot, retroarch
NOTES.md          Hardware and development notes
```
