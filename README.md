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

ROMs go in `/roms` on the card. The system itself is `/boot/rootfs.ext4`,
an ext4 image mounted read-write as the root, so `apk add` and edits under
`/etc` survive a reboot; the card stays exFAT for the PC.

## Updates

The System update app (Apps) runs `apk update` and `apk upgrade` on the
device: Alpine's packages are installed by apk and everything of ours is a
package too (`s30-mesa`, `s30-tools`, `s30-retroarch`,
`s30-emulationstation`, `s30-linux`, `s30-uboot`, `s30-base`), so the
database covers the whole system. Our packages come from this repository's
`gh-pages` branch (GitHub Actions builds and publishes on every push to
main, `.github/workflows/packages.yml`; by hand `bash build/build.sh
publish` after `initramfs`, from a committed tree), signed with
`build/s30.rsa` (the Actions secret `S30_SIGN_KEY`); a new kernel, loader or
U-Boot is copied to the card by the `s30-base` trigger (`boot-update`) at
the end of the upgrade. The SPI flash (U-Boot and a fallback kernel) is
only rewritten by hand (`build.sh flash`, `s30-flash write`). It needs internet over the USB
cable: share the PC's connection
with the device's network interface (NetworkManager: "Shared to other
computers", which needs dnsmasq installed; Windows: Internet Connection
Sharing; macOS: Internet Sharing), then plug the device in. Without
sharing the device still hands the PC an address itself, so ssh to
`s30.local` works either way.

## Layout

```
build/            build.sh, kernel config, U-Boot board files,
                  initramfs loader, rootfs overlay (init, launcher apps,
                  RetroArch and EmulationStation config), boot scripts,
                  splash images
linux/            Board devicetree, panel and gamepad drivers
patches/          Patches to the submodules, one folder each: linux,
                  u-boot, retroarch, emulationstation
launcher/         s30-launcher (file browser)
utils/            Small system tools (volume daemon, splash, FEL reboot, ...)
tools/            Firmware decoding and host-side helpers
external/         Submodules: linux, u-boot, retroarch, emulationstation,
                  es-theme-carbon
NOTES.md          Hardware and development notes
```
