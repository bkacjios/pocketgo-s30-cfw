#!/bin/bash
# Build a bootable PocketGo S30 SD card image (mainline U-Boot + Linux + Alpine).
#
# Run on a Debian/Ubuntu or Arch host:  bash build/build.sh
#
# Linux, U-Boot and RetroArch are git submodules (external/, pinned
# versions); each is built in a git worktree under $WORK (Linux filesystem;
# the kernel tree cannot be checked out on NTFS), so board patches never touch
# the submodule checkout. The results go to $WORK (s30-build/); build.sh
# sdcard writes them to a card.
#
# Steps can be run individually: build.sh deps|linux|uboot|rootfs|initramfs
# USB boot from stock firmware (no flashing): build.sh initramfs, then fel
# Our programs, the kernel, U-Boot and the board files are apk packages
# (signed with build/s30.rsa); build.sh publish puts them online for the
# device's System update app.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(dirname "$HERE")"
WORK="${WORK:-$PROJECT/s30-build}"
OUT="${OUT:-$PROJECT/out}"
JOBS="$(nproc)"
export ARCH=arm
# Timestamps in what mkimage (uInitrd, the FIT, the scripts) and the kernel
# and U-Boot builds embed: the commit's, so a rebuild of one commit gives the
# same files (boot-update compares them with what is installed)
export SOURCE_DATE_EPOCH
SOURCE_DATE_EPOCH="$(git -C "$PROJECT" log -1 --format=%ct)"

ALPINE_MIRROR=https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/armv7
ALPINE_REPO=https://dl-cdn.alpinelinux.org/alpine/latest-stable
# Our package repository: $WORK/repo, published as the gh-pages branch of
# origin (build.sh publish), which GitHub Pages serves here. Listed in the
# device's /etc/apk/repositories; the packages are signed with SIGN_KEY
# (private, gitignored; made on first use) and PUB_KEY (committed, in the
# rootfs as /etc/apk/keys)
S30_APK_REPO=https://bkacjios.github.io/pocketgo-s30-cfw/apk
SIGN_KEY="$HERE/s30.rsa"
PUB_KEY="$HERE/rootfs-overlay/etc/apk/keys/s30.rsa.pub"

mkdir -p "$WORK" "$OUT"

# Build tree $WORK/<name> for submodule external/<name>: a detached git
# worktree at the pinned commit (fetched on first use).
upstream_tree() {
	local name="$1" src="$WORK/$1" sub="$PROJECT/external/$1" pinned
	[ -e "$sub/.git" ] || git -C "$PROJECT" submodule update --init --depth 1 "external/$name"
	pinned="$(git -C "$sub" rev-parse HEAD)"
	if [ ! -e "$src/.git" ]; then
		git -C "$sub" worktree prune
		git -C "$sub" worktree add --detach "$src" "$pinned"
	fi
	if [ "$(git -C "$src" rev-parse HEAD)" != "$pinned" ]; then
		echo "$src is not at the commit external/$name pins ($pinned); delete it to rebuild" >&2
		exit 1
	fi
	echo "$name: $(git -C "$src" describe --tags)"
}

# Pick the ARM cross toolchain prefix: $CROSS_COMPILE if set, else the first
# installed of the Debian (gnueabihf) or Arch (none-eabi) toolchains. The
# kernel and U-Boot are freestanding, so a bare-metal toolchain works.
set_cross() {
	if [ -z "${CROSS_COMPILE:-}" ]; then
		local p
		for p in arm-linux-gnueabihf- arm-none-eabi-; do
			command -v "${p}gcc" >/dev/null && { CROSS_COMPILE=$p; break; }
		done
	fi
	[ -n "${CROSS_COMPILE:-}" ] || { echo "no ARM cross compiler found; run: $0 deps" >&2; exit 1; }
	export CROSS_COMPILE
	echo "Toolchain: $("${CROSS_COMPILE}gcc" --version | head -n1)"
}

step_deps() {
	if command -v pacman >/dev/null; then
		sudo pacman -S --needed --noconfirm \
			git make gcc bc bison flex openssl libelf ncurses \
			arm-none-eabi-gcc arm-none-eabi-binutils dtc swig python \
			python-setuptools python-pyelftools gnutls util-linux-libs \
			e2fsprogs util-linux fakeroot curl ca-certificates cpio kmod \
			clang lld llvm squashfs-tools pkgconf cmake ninja
		return
	fi
	sudo apt-get update
	sudo apt-get install -y --no-install-recommends \
		git make gcc bc bison flex libssl-dev libelf-dev libncurses-dev \
		gcc-arm-linux-gnueabihf device-tree-compiler swig python3-dev \
		python3-setuptools python3-pyelftools libgnutls28-dev uuid-dev \
		e2fsprogs fdisk fakeroot curl ca-certificates cpio kmod \
		clang lld llvm squashfs-tools pkg-config cmake ninja-build python3-venv
}

step_linux() {
	set_cross
	local src="$WORK/linux"
	upstream_tree linux

	# Kernel patches (idempotent: skip if already applied).
	local patch
	for patch in "$PROJECT"/patches/linux/*.patch; do
		git -C "$src" apply --reverse --check "$patch" 2>/dev/null ||
			git -C "$src" apply "$patch"
	done

	# Board files (idempotent). Each driver directory under linux/drivers has
	# its sources plus Kconfig.s30 (inserted before the Kconfig's closing
	# endif/endmenu) and Makefile.s30 (appended to the Makefile).
	cp "$PROJECT/linux/arch/arm/boot/dts/allwinner/sun8i-a33-pocketgo-s30.dts" \
		"$src/arch/arm/boot/dts/allwinner/"
	# Boot logo (built from build/splash/logo.bmp; see kernel.config)
	cp "$PROJECT"/linux/drivers/video/logo/*.ppm "$src/drivers/video/logo/"
	local frag dir sym
	for frag in $(cd "$PROJECT/linux" && find drivers -name Kconfig.s30); do
		dir="$(dirname "$frag")"
		cp "$PROJECT/linux/$dir/"*.c "$src/$dir/"
		sym="$(head -n1 "$PROJECT/linux/$frag")"
		if ! grep -qx "$sym" "$src/$dir/Kconfig"; then
			python3 - "$src/$dir/Kconfig" "$PROJECT/linux/$frag" <<-'EOF'
			import re, sys
			kconfig, frag = sys.argv[1:]
			s = open(kconfig).read()
			end = list(re.finditer(r'^end(if|menu)\b', s, re.M))[-1].start()
			open(kconfig, 'w').write(s[:end] + open(frag).read() + s[end:])
			EOF
			cat "$PROJECT/linux/$dir/Makefile.s30" >> "$src/$dir/Makefile"
		fi
	done
	if ! grep -q pocketgo-s30 "$src/arch/arm/boot/dts/allwinner/Makefile"; then
		sed -i '0,/^dtb-$(CONFIG_MACH_SUN8I) += \\/s//&\n\tsun8i-a33-pocketgo-s30.dtb \\/' \
			"$src/arch/arm/boot/dts/allwinner/Makefile"
		grep -q pocketgo-s30 "$src/arch/arm/boot/dts/allwinner/Makefile" ||
			{ echo "could not add dtb to allwinner/Makefile" >&2; exit 1; }
	fi

	make -C "$src" sunxi_defconfig
	"$src/scripts/kconfig/merge_config.sh" -m -O "$src" "$src/.config" "$HERE/kernel.config"
	make -C "$src" olddefconfig
	# Fail loudly if a required option was dropped by olddefconfig.
	while IFS= read -r line; do
		case "$line" in CONFIG_*=y)
			grep -qx "$line" "$src/.config" || { echo "missing in .config: $line" >&2; exit 1; } ;;
		esac
	done < "$HERE/kernel.config"

	make -C "$src" -j"$JOBS" zImage allwinner/sun8i-a33-pocketgo-s30.dtb
	install -D "$src/arch/arm/boot/zImage" "$WORK/boot/zImage"
	install -D "$src/arch/arm/boot/dts/allwinner/sun8i-a33-pocketgo-s30.dtb" \
		"$WORK/boot/sun8i-a33-pocketgo-s30.dtb"
}

step_uboot() {
	set_cross
	local src="$WORK/u-boot"
	upstream_tree u-boot

	# Board patches (idempotent: skip if already applied).
	local patch
	for patch in "$PROJECT"/patches/u-boot/*.patch; do
		git -C "$src" apply --reverse --check "$patch" 2>/dev/null ||
			git -C "$src" apply "$patch"
	done

	cp "$HERE/u-boot/pocketgo_s30_defconfig" "$src/configs/"
	if [ -f "$src/arch/arm/dts/sun8i-a33.dtsi" ]; then
		# U-Boot's own dts directory.
		cp "$HERE/u-boot/sun8i-a33-pocketgo-s30.dts" "$src/arch/arm/dts/"
		grep -q pocketgo-s30 "$src/arch/arm/dts/Makefile" ||
			sed -i '0,/^dtb-$(CONFIG_MACH_SUN8I_A33) += \\/s//&\n\tsun8i-a33-pocketgo-s30.dtb \\/' \
				"$src/arch/arm/dts/Makefile"
		sed -i '/^CONFIG_OF_UPSTREAM/d' "$src/configs/pocketgo_s30_defconfig"
		echo '# CONFIG_OF_UPSTREAM is not set' >> "$src/configs/pocketgo_s30_defconfig"
	else
		# Upstream (Linux) devicetree layout.
		cp "$HERE/u-boot/sun8i-a33-pocketgo-s30.dts" \
			"$src/dts/upstream/src/arm/allwinner/"
		sed -i 's|^CONFIG_DEFAULT_DEVICE_TREE=.*|CONFIG_DEFAULT_DEVICE_TREE="allwinner/sun8i-a33-pocketgo-s30"|' \
			"$src/configs/pocketgo_s30_defconfig"
	fi

	make -C "$src" pocketgo_s30_defconfig
	make -C "$src" -j"$JOBS"
	install -D "$src/u-boot-sunxi-with-spl.bin" "$WORK/u-boot-sunxi-with-spl.bin"
	mkdir -p "$WORK/boot"
	"$src/tools/mkimage" -A arm -O linux -T script -C none \
		-d "$HERE/boot.cmd" "$WORK/boot/boot.scr"
}

step_rootfs() {
	local ver tarball
	ver="$(curl -fsSL "$ALPINE_MIRROR/latest-releases.yaml" |
		awk '/flavor: alpine-minirootfs/{f=1} f&&/file:/{print $2; exit}')"
	tarball="$WORK/$ver"
	[ -f "$tarball" ] || curl -fSL -o "$tarball" "$ALPINE_MIRROR/$ver"
	echo "Rootfs: $ver"
	echo "$tarball" > "$WORK/rootfs.src"

	# apk for the build host (static, from Alpine's own repo): it installs
	# build/packages into the rootfs tree (step_initramfs), so the device's
	# package database is complete and apk upgrade there covers everything
	local host_arch apk_ver
	host_arch="$(uname -m)"
	apk_ver="$(curl -fsSL "$ALPINE_REPO/main/$host_arch/APKINDEX.tar.gz" | tar -xzO APKINDEX |
		awk '/^P:apk-tools-static$/ { f = 1 } f && /^V:/ { v = substr($0, 3); f = 0 } END { print v }')"
	[ -n "$apk_ver" ] || { echo "no apk-tools-static for $host_arch" >&2; exit 1; }
	mkdir -p "$WORK/bin" "$WORK/apk"
	# Downloaded whole: tar stops at the one file and the pipe would break
	curl -fsSL -o "$WORK/apk-tools-static.apk" "$ALPINE_REPO/main/$host_arch/apk-tools-static-$apk_ver.apk"
	tar -xzOf "$WORK/apk-tools-static.apk" sbin/apk.static > "$WORK/bin/apk.static"
	chmod 0755 "$WORK/bin/apk.static"
	# Left by the old resolver (tools/apk_resolve.py)
	rm -f "$WORK"/apk/APKINDEX.main "$WORK"/apk/APKINDEX.community "$WORK"/apk/installed \
		"$WORK"/apk/list "$WORK"/apk/*-r[0-9]*.apk
	echo "apk-tools-static $apk_ver"
}

# Cross sysroot for clang builds (RetroArch, launcher): Alpine minirootfs +
# toolchain runtime (gcc for crt/libgcc) + -dev packages, and single-word
# compiler wrappers $WORK/bin/armv7-clang[++]. Rebuilt when the list changes.
SYSROOT_PKGS="gcc g++ musl-dev linux-headers libdrm-dev mesa-dev alsa-lib-dev eudev-dev zlib-dev sdl2-dev sdl2_ttf-dev
	freeimage-dev freetype-dev curl-dev vlc-dev rapidjson-dev pugixml-dev"
ensure_sysroot() {
	local sr="$WORK/sysroot" t
	if [ "$(cat "$sr/.done" 2>/dev/null)" != "$SYSROOT_PKGS" ]; then
		rm -rf "$sr" "$WORK/sysroot-apk"; mkdir -p "$sr" "$WORK/sysroot-apk"
		tar -xzf "$(cat "$WORK/rootfs.src")" -C "$sr"
		# shellcheck disable=SC2086
		fakeroot "$WORK/bin/apk.static" --root "$sr" --arch armv7 --no-scripts --no-interactive \
			--cache-dir "$WORK/sysroot-apk" --keys-dir "$sr/etc/apk/keys" \
			--repository "$ALPINE_REPO/main" --repository "$ALPINE_REPO/community" \
			add $SYSROOT_PKGS
		echo "$SYSROOT_PKGS" > "$sr/.done"
	fi
	mkdir -p "$WORK/bin"
	for t in clang clang++; do
		printf '#!/bin/sh\nexec %s --target=armv7-alpine-linux-musleabihf --sysroot=%s -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -fuse-ld=lld "$@"\n' \
			"$t" "$sr" > "$WORK/bin/armv7-$t"
		chmod +x "$WORK/bin/armv7-$t"
	done
}

# Device programs. The launcher (launcher/): filebrowser.c, the boot UI that
# starts RetroArch, OoT and other apps (a tile grid with icons, an image
# viewer), and flash.c (s30-flash, the boot flash writer), with ui.c. SDL2 (KMS), text in utils/osd_font.h, images through
# the vendored stb_image.h. Utilities (utils/): s30-fel, s30-volumed, s30-splash,
# s30-keyheld, s30-display (with the system menu and stick calibration) and its
# preload libs30lease.so, s30-screenshot (the screen's KMS planes to a PNG),
# s30-uvc and s30-uac (the screen and the speaker's sound as a USB webcam and
# sound card), s30-mdns (s30.local over the USB network). Output:
# $WORK/{launcher,s30-*}.bin, $WORK/libs30lease.so
step_launcher() {
	local app
	ensure_sysroot
	for app in filebrowser:launcher flash:s30-flash; do
		"$WORK/bin/armv7-clang" -O2 -Wall -o "$WORK/${app#*:}.bin" \
			"$PROJECT/launcher/${app%%:*}.c" "$PROJECT/launcher/ui.c" \
			-I"$WORK/sysroot/usr/include/SDL2" -lSDL2 -lm
		llvm-strip "$WORK/${app#*:}.bin"
		ls -l "$WORK/${app#*:}.bin"
	done
	# Small system utilities (utils/)
	"$WORK/bin/armv7-clang" -O2 -Wall -o "$WORK/s30-fel.bin" "$PROJECT/utils/s30-fel.c"
	"$WORK/bin/armv7-clang" -O2 -Wall -o "$WORK/s30-volumed.bin" \
		"$PROJECT/utils/s30-volumed.c" -lasound
	"$WORK/bin/armv7-clang" -O2 -Wall -o "$WORK/s30-splash.bin" "$PROJECT/utils/s30-splash.c"
	"$WORK/bin/armv7-clang" -O2 -Wall -o "$WORK/s30-keyheld.bin" "$PROJECT/utils/s30-keyheld.c"
	"$WORK/bin/armv7-clang" -O2 -Wall -o "$WORK/s30-display.bin" "$PROJECT/utils/s30-display.c" \
		-I"$WORK/sysroot/usr/include/libdrm" -ldrm -lm
	"$WORK/bin/armv7-clang" -O2 -Wall -o "$WORK/s30-screenshot.bin" "$PROJECT/utils/s30-screenshot.c" \
		-I"$WORK/sysroot/usr/include/libdrm" -ldrm -lz
	"$WORK/bin/armv7-clang" -O2 -Wall -o "$WORK/s30-uvc.bin" "$PROJECT/utils/s30-uvc.c" \
		-I"$WORK/sysroot/usr/include/libdrm" -ldrm
	"$WORK/bin/armv7-clang" -O2 -Wall -o "$WORK/s30-uac.bin" "$PROJECT/utils/s30-uac.c" -lasound
	"$WORK/bin/armv7-clang" -O2 -Wall -o "$WORK/s30-mdns.bin" "$PROJECT/utils/s30-mdns.c"
	"$WORK/bin/armv7-clang" -O2 -Wall -fPIC -shared -o "$WORK/libs30lease.so" "$PROJECT/utils/s30-lease.c"
	llvm-strip "$WORK/s30-fel.bin" "$WORK/s30-volumed.bin" "$WORK/s30-splash.bin" \
		"$WORK/s30-keyheld.bin" "$WORK/s30-display.bin" "$WORK/s30-screenshot.bin" "$WORK/s30-uvc.bin" "$WORK/s30-uac.bin" \
		"$WORK/s30-mdns.bin" "$WORK/libs30lease.so"
}

# RetroArch, cross-compiled with clang for armv7 musl against an Alpine
# sysroot (Alpine -dev packages, resolved like build/packages), lean config
# (KMS/EGL/GLES2, ALSA, udev) plus patches/retroarch. Output: $WORK/retroarch.bin
step_retroarch() {
	local src="$WORK/retroarch" sr="$WORK/sysroot"
	local idx name ver repo patch t
	upstream_tree retroarch
	for patch in "$PROJECT"/patches/retroarch/*.patch; do
		git -C "$src" apply --reverse --check "$patch" 2>/dev/null ||
			git -C "$src" apply "$patch"
	done

	ensure_sysroot

	(
		cd "$src"
		export CC="$WORK/bin/armv7-clang" CXX="$WORK/bin/armv7-clang++"
		export PKG_CONF_PATH=pkg-config PKG_CONFIG_SYSROOT_DIR="$sr"
		export PKG_CONFIG_LIBDIR="$sr/usr/lib/pkgconfig:$sr/usr/share/pkgconfig"
		./configure --host=armv7-alpine-linux-musleabihf --prefix=/usr \
			--disable-x11 --disable-wayland --disable-qt --disable-pulse \
			--disable-ffmpeg --disable-vulkan --disable-sdl --disable-sdl2 \
			--disable-discord --disable-opengl1 --disable-opengl_core \
			--enable-opengles --enable-kms --enable-egl --enable-alsa \
			--enable-udev --disable-oss --disable-jack --disable-v4l2 \
			--disable-freetype --disable-cg --enable-neon
		make -j"$JOBS"
	)
	llvm-strip -o "$WORK/retroarch.bin" "$src/retroarch"
	ls -l "$WORK/retroarch.bin"
}

# EmulationStation (RetroPie's), cross-compiled with clang for armv7 musl
# against the Alpine sysroot: Mesa GLES 2.0 renderer (the Mali-400 has no
# GLES 3), Alpine's pugixml (not the bundled submodule), plus
# patches/emulationstation (0001 from the AUR package: install rules,
# resources in /usr/share/EmulationStation). Output: installed under
# $WORK/emulationstation-root (/usr/bin/emulationstation, its resources)
step_emulationstation() {
	local src="$WORK/emulationstation" sr="$WORK/sysroot" patch
	upstream_tree emulationstation
	for patch in "$PROJECT"/patches/emulationstation/*.patch; do
		[ -e "$patch" ] || continue
		git -C "$src" apply --reverse --check "$patch" 2>/dev/null ||
			git -C "$src" apply "$patch"
	done

	ensure_sysroot

	PKG_CONFIG_SYSROOT_DIR="$sr" \
	PKG_CONFIG_LIBDIR="$sr/usr/lib/pkgconfig:$sr/usr/share/pkgconfig" \
	cmake -S "$src" -B "$src/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=armv7 \
		-DCMAKE_C_COMPILER="$WORK/bin/armv7-clang" -DCMAKE_CXX_COMPILER="$WORK/bin/armv7-clang++" \
		-DCMAKE_SYSROOT="$sr" -DCMAKE_FIND_ROOT_PATH="$sr" \
		-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
		-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
		-DCMAKE_STRIP="$(command -v llvm-strip)" -DCMAKE_INSTALL_PREFIX=/usr -DUSE_MESA_GLES=ON \
		-DCMAKE_CXX_FLAGS=-D_LARGEFILE64_SOURCE # musl >= 1.2.4 hides stat64 otherwise
	cmake --build "$src/build" -j"$JOBS"
	rm -rf "$WORK/emulationstation-root"
	DESTDIR="$WORK/emulationstation-root" cmake --install "$src/build" --strip
	ls -l "$WORK/emulationstation-root/usr/bin/emulationstation"
}

# Mesa for the S30 only: the lima GPU driver (and kmsro for the sun4i-drm
# display) with EGL, GBM and GLES 2, no LLVM. Alpine's mesa-dri-gallium is
# one libgallium with every driver, linked to libLLVM (145 MB): loading it
# took 3.5 s of the launcher's start at boot (squashfs reads and
# decompression, musl relocates the whole library). The same release as
# Alpine's (its headers are in the sysroot), checked against Alpine 3.24's
# sha512, plus patches/mesa. Build tools (meson, mako, PyYAML) in a venv under $WORK.
# Output: installed under $WORK/mesa-root, which replaces Alpine's mesa
# packages in the rootfs (build/packages excludes them).
MESA_VERSION=26.1.6
MESA_SHA512=6746b73f5b87e8ff3723eee586b4b5f033dbe6d13cd0309d743b7fa484c350268859ec8ff21aadcbca7a04070cdaa7828146dd28dde4b1935bb879891d687f0b
step_mesa() {
	local dir="$WORK/mesa-src" src="$WORK/mesa-src/mesa-$MESA_VERSION" sr="$WORK/sysroot" venv="$WORK/mesa-venv" patch
	local tarball="$WORK/mesa-src/mesa-$MESA_VERSION.tar.xz"
	ensure_sysroot
	mkdir -p "$dir"
	[ -f "$tarball" ] || curl -fsSL -o "$tarball" "https://archive.mesa3d.org/mesa-$MESA_VERSION.tar.xz"
	echo "$MESA_SHA512  $tarball" | sha512sum -c --quiet
	[ -d "$src" ] || tar -xJf "$tarball" -C "$dir"
	for patch in "$PROJECT"/patches/mesa/*.patch; do
		[ -e "$patch" ] || continue
		patch -d "$src" -p1 -R -s -f --dry-run < "$patch" >/dev/null 2>&1 ||
			patch -d "$src" -p1 -s < "$patch"
	done
	if [ ! -x "$venv/bin/meson" ]; then
		python3 -m venv "$venv"
		"$venv/bin/pip" install --quiet meson mako pyyaml packaging
	fi
	cat > "$dir/cross.ini" <<-EOF
	[binaries]
	c = '$WORK/bin/armv7-clang'
	cpp = '$WORK/bin/armv7-clang++'
	ar = '$(command -v llvm-ar)'
	strip = '$(command -v llvm-strip)'
	pkg-config = 'pkg-config'
	[built-in options]
	# The wrapper's -fuse-ld=lld is unused in compile-only checks, which
	# meson runs with -Werror=unused-command-line-argument
	c_args = ['-Qunused-arguments']
	cpp_args = ['-Qunused-arguments']
	[properties]
	sys_root = '$sr'
	pkg_config_libdir = '$sr/usr/lib/pkgconfig:$sr/usr/share/pkgconfig'
	[host_machine]
	system = 'linux'
	cpu_family = 'arm'
	cpu = 'armv7'
	endian = 'little'
	EOF
	rm -rf "$src/build"
	PKG_CONFIG_SYSROOT_DIR="$sr" PATH="$venv/bin:$PATH" \
	meson setup "$src/build" "$src" --cross-file "$dir/cross.ini" \
		--prefix=/usr --buildtype=release -Db_ndebug=true -Dstrip=true \
		-Dplatforms= -Dgallium-drivers=lima -Dvulkan-drivers= -Dvulkan-layers= \
		-Dllvm=disabled -Dglx=disabled -Dglvnd=disabled -Dopengl=true -Dgles1=disabled -Dgles2=enabled \
		-Degl=enabled -Dgbm=enabled -Dshader-cache=enabled -Dzstd=disabled -Dxmlconfig=disabled \
		-Dexpat=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
		-Dgallium-va=disabled -Dgallium-rusticl=false -Dvideo-codecs= -Dmicrosoft-clc=disabled \
		-Dspirv-tools=disabled -Dbuild-tests=false -Dtools= -Dallow-kcmp=enabled
	PATH="$venv/bin:$PATH" meson compile -C "$src/build" -j "$JOBS"
	rm -rf "$WORK/mesa-root"
	DESTDIR="$WORK/mesa-root" PATH="$venv/bin:$PATH" meson install -C "$src/build" --no-rebuild --quiet
	# Headers and pkg-config files are the sysroot's (Alpine's mesa-dev)
	rm -rf "$WORK/mesa-root/usr/include" "$WORK/mesa-root/usr/lib/pkgconfig" "$WORK/mesa-root/usr/share"
	du -sh "$WORK/mesa-root"
}

# gmloader-next (PortsMaster's): runs GameMaker Android titles (AM2R) natively,
# cross-compiled with clang for armv7 musl against the Alpine sysroot, plus
# patches/gmloader-next (musl and bionic armv7 ABI wrappers, the host libm for
# the ARMv8-only AOSP one, the rotation for the portrait panel). Its libc
# table generator parses the sysroot's headers with libclang (bindings in a
# venv under $WORK). Output: $WORK/gmloader/ (gmloadernext.armhf and the
# lib/armeabi-v7a libraries it loads; see build/gmloader/am2r)
step_gmloader() {
	local src="$WORK/gmloader-next" sr="$WORK/sysroot" venv="$WORK/gml-venv" patch cxxv out="$WORK/gmloader"
	upstream_tree gmloader-next
	git -C "$src" submodule update --init --recursive --depth 1 -q
	for patch in "$PROJECT"/patches/gmloader-next/*.patch; do
		[ -e "$patch" ] || continue
		git -C "$src" apply --reverse --check "$patch" 2>/dev/null ||
			git -C "$src" apply "$patch"
	done
	ensure_sysroot
	if [ ! -x "$venv/bin/python3" ]; then
		python3 -m venv "$venv"
		"$venv/bin/pip" install --quiet libclang==18.1.1
	fi
	cxxv="$(ls "$sr/usr/include/c++" | head -1)"
	PATH="$venv/bin:$PATH" \
	GML_CLANG_EXTRA="--sysroot=$sr --target=armv7-alpine-linux-musleabihf -march=armv7-a -mfloat-abi=hard \
-isystem $sr/usr/include/c++/$cxxv -isystem $sr/usr/include/c++/$cxxv/armv7-alpine-linux-musleabihf \
-I$src/s30 -D_LARGEFILE64_SOURCE" \
	PKG_CONFIG_SYSROOT_DIR="$sr" PKG_CONFIG_LIBDIR="$sr/usr/lib/pkgconfig" \
	make -C "$src" -f Makefile.gmloader ARCH=arm-linux-gnueabihf \
		CC="$WORK/bin/armv7-clang" CXX="$WORK/bin/armv7-clang++" PKG_CONFIG=pkg-config \
		LLVM_INC="$sr/usr/include" OPTM="-Os -I$src/s30 -D_LARGEFILE64_SOURCE -Wno-c++11-narrowing" -j"$JOBS"
	rm -rf "$out"
	mkdir -p "$out/lib/armeabi-v7a"
	install -m 0755 "$src/build/arm-linux-gnueabihf/gmloader/gmloadernext.armhf" "$out/"
	llvm-strip "$out/gmloadernext.armhf"
	# Not libm.so: ARMv8-A code (the host's libm serves it)
	install -m 0644 "$src/lib/armeabi-v7a/libc++_shared.so" "$src/lib/armeabi-v7a/libcompiler_rt.so" \
		"$out/lib/armeabi-v7a/"
	ls -l "$out/gmloadernext.armhf"
}

# Root filesystem (minirootfs + packages) as rootfs.ext4 (an ext4 image,
# mounted read-write as / on the device: changes persist, apk add works) and
# rootfs.sqfs (squashfs, the read-only fallback under a tmpfs overlay; what
# build.sh fel pushes). uInitrd is a small loader (busybox, musl, s30-loader
# as /init) that mounts one of them from /boot on the SD card.
#
# Everything of ours is a package too (apk mkpkg, signed with SIGN_KEY), so
# apk knows every file, checks dependencies and apk upgrade on the device
# (the System update app) takes newer ones from S30_APK_REPO:
#   s30-mesa              build.sh mesa (lima, no LLVM); provides Alpine's
#                         mesa-* names so nothing pulls Alpine's Mesa
#   s30-tools             the launcher, utils/, libs30lease, splash.bmp
#   s30-retroarch, s30-emulationstation (with the theme)
#   s30-linux             zImage, DTB, the loader (uInitrd) and boot.scr, in
#                         /usr/share/s30/boot
#   s30-uboot             u-boot-sunxi-with-spl.bin, same place
#   s30-base              rootfs-overlay (init, configs, scripts); its apk
#                         trigger on /usr/share/s30/boot runs boot-update,
#                         which copies the new boot files to /boot on the
#                         card (and U-Boot to 8 KiB when the card has one).
#                         The SPI flash (U-Boot, fallback kernel) is only
#                         written by hand: build.sh flash, s30-flash write
# Versions: 0.<commit count>-r0 (s30-linux and s30-uboot: the upstream
# version, -r<commit count>), so a later commit always upgrades; the
# description names the commit.
ROOTFS_EXT4_MB=1024 # the image's size; the tree is ~300 MB, the rest is free for apk add
PKG_REL= PKG_DESC= # commit count, git describe (step_initramfs)
MKPKG_DEPS=        # extra depends for the next mkpkg
sign_key() {
	[ -f "$SIGN_KEY" ] && return
	if [ -f "$PUB_KEY" ]; then
		echo "$SIGN_KEY is missing but $PUB_KEY exists: the devices only take packages signed with that key." >&2
		echo "Restore it, or delete the public key for a new pair (and reflash the devices)." >&2
		exit 1
	fi
	mkdir -p "${PUB_KEY%/*}"
	openssl genrsa -out "$SIGN_KEY" 2048 2>/dev/null
	openssl rsa -in "$SIGN_KEY" -pubout -out "$PUB_KEY"
	echo "New package signing key $SIGN_KEY; commit its public key $PUB_KEY"
}
mkpkg() { # NAME VERSION DIR [--info ...|--trigger ...|--script ...]: $WORK/repo/armv7/NAME-VERSION.apk
	local name="$1" version="$2" dir="$3" deps
	shift 3
	# The libraries the programs link, minus libc and the package's own
	# (|| true: grep finds nothing in a package without ELF files, pipefail)
	deps="$(find "$dir" -type f -exec sh -c 'readelf -h "$1" >/dev/null 2>&1 && readelf -d "$1"' _ {} \; |
		awk '/NEEDED/ { gsub(/[][]/, "", $5); print $5 }' | sort -u |
		grep -v -E '^(libc\.musl|ld-musl)' |
		grep -v -x -F -f <(find "$dir" -name '*.so*' -printf '%f\n') |
		sed 's/^/so:/' | tr '\n' ' ' || true)$MKPKG_DEPS"
	"$WORK/bin/apk.static" mkpkg --files "$dir" --output "$WORK/repo/armv7/$name-$version.apk" \
		--sign-key "$SIGN_KEY" --info "name:$name" --info "version:$version" --info "arch:armv7" \
		--info "depends:$deps" --info "description:PocketGo S30 $name ($PKG_DESC)" "$@"
	MKPKG_DEPS=
}
mkndx() { # the repository's index over every package built so far
	"$WORK/bin/apk.static" --keys-dir "${PUB_KEY%/*}" mkndx --sign-key "$SIGN_KEY" \
		--output "$WORK/repo/armv7/APKINDEX.tar.gz" --description "PocketGo S30 $PKG_DESC" \
		"$WORK"/repo/armv7/*.apk > /dev/null
}
step_initramfs() {
	local root="$WORK/rootfs" loader="$WORK/loader" pkg="$WORK/pkg" mesa_ver d t
	sign_key
	rm -rf "$root" "$loader" "$pkg" "$WORK/repo"; mkdir -p "$root" "$loader" "$pkg" "$WORK/repo/armv7"
	tar -xzf "$(cat "$WORK/rootfs.src")" -C "$root"
	cp "$PUB_KEY" "$root/etc/apk/keys/"
	echo "$S30_APK_REPO" >> "$root/etc/apk/repositories"
	# Packages through apk (downloads cached in $WORK/apk). fakeroot: apk
	# chowns what it installs. --no-scripts: the packages' install scripts
	# and triggers are ARM programs; none of ours needs them at build time
	apk() {
		fakeroot "$WORK/bin/apk.static" --root "$root" --arch armv7 --no-scripts --no-interactive \
			--cache-dir "$WORK/apk" --keys-dir "$root/etc/apk/keys" \
			--repository "$ALPINE_REPO/main" --repository "$ALPINE_REPO/community" \
			--repository "$WORK/repo" "$@"
	}
	PKG_REL="$(git -C "$PROJECT" rev-list --count HEAD)"
	PKG_DESC="$(git -C "$PROJECT" describe --always --dirty)"
	# Our Mesa (build.sh mesa) first: it provides Alpine's mesa-* names and
	# the EGL/GBM/GLES libraries, so what needs them (kmscube) takes ours
	# and apk never installs Alpine's Mesa (with LLVM)
	if [ -d "$WORK/mesa-root" ]; then
		mesa_ver="$(ls "$WORK"/mesa-root/usr/lib/libgallium-*.so | sed 's/.*libgallium-\(.*\)\.so/\1/')"
		mkpkg s30-mesa "$mesa_ver-r0" "$WORK/mesa-root" \
			--info "description:Mesa $mesa_ver for the Mali-400 (lima; EGL, GBM, GLES 2; no LLVM)" \
			--info "provides:so:libEGL.so.1=1 so:libgbm.so.1=1 so:libGLESv2.so.2=2 mesa=$mesa_ver-r0 mesa-egl=$mesa_ver-r0 mesa-gbm=$mesa_ver-r0 mesa-gles=$mesa_ver-r0 mesa-dri-gallium=$mesa_ver-r0" \
			--info "provider-priority:100"
		mkndx
		apk add s30-mesa
	fi
	d="$pkg/s30-tools"
	for t in launcher:s30-launcher s30-fel s30-flash s30-volumed s30-splash s30-keyheld \
			s30-display s30-screenshot s30-uvc s30-uac s30-mdns; do
		[ ! -f "$WORK/${t%%:*}.bin" ] ||
			install -D -m 0755 "$WORK/${t%%:*}.bin" "$d/usr/bin/${t##*:}"
	done
	[ ! -f "$WORK/libs30lease.so" ] ||
		install -D -m 0755 "$WORK/libs30lease.so" "$d/usr/lib/s30/libs30lease.so"
	# Boot splash: the same image U-Boot shows
	install -D -m 0644 "$HERE/splash/logo.bmp" "$d/usr/share/s30/splash.bmp"
	[ ! -f "$WORK/retroarch.bin" ] ||
		install -D -m 0755 "$WORK/retroarch.bin" "$pkg/s30-retroarch/usr/bin/retroarch"
	if [ -d "$WORK/emulationstation-root" ]; then
		cp -r "$WORK/emulationstation-root" "$pkg/s30-emulationstation"
		# Theme (submodule external/es-theme-carbon)
		if [ -f "$PROJECT/external/es-theme-carbon/carbon.xml" ]; then
			d="$pkg/s30-emulationstation/etc/emulationstation/themes/carbon"
			mkdir -p "$d"
			tar -C "$PROJECT/external/es-theme-carbon" -cf - --exclude=.git . | tar -C "$d" -xf -
		fi
	fi
	for d in "$pkg"/*/; do
		d="${d%/}"
		mkpkg "$(basename "$d")" "0.$PKG_REL-r0" "$d"
	done
	mkndx
	# shellcheck disable=SC2046
	apk add $(grep -v '^#' "$HERE/packages") s30-tools s30-retroarch s30-emulationstation

	# The loader (initramfs): busybox and musl from the rootfs
	mkdir -p "$loader"/{bin,lib,dev,proc,sys}
	cp -a "$root/bin/busybox" "$loader/bin/"
	cp -a "$root"/lib/ld-musl-armhf.so.1 "$loader/lib/"
	# Card check (dosfstools, exfatprogs; libc only) and e2fsck for rootfs.ext4
	# with its libraries (libblkid pulls in libeconf, that libgcc_s); every
	# NEEDED checked, a missing one only shows at boot
	cp -a "$root/sbin/fsck.fat" "$root/usr/sbin/fsck.exfat" "$root/sbin/e2fsck" "$loader/bin/"
	mkdir -p "$loader/usr/lib"
	cp -a "$root"/usr/lib/lib{blkid,com_err,e2p,ext2fs,uuid,econf,gcc_s}.so.* "$loader/usr/lib/"
	for so in $(readelf -d "$loader"/bin/e2fsck "$loader"/usr/lib/*.so.* |
			awk '/NEEDED/ { gsub(/[][]/, "", $5); print $5 }' | sort -u); do
		[ "$so" = libc.musl-armv7.so.1 ] || [ -e "$loader/usr/lib/$so" ] ||
			{ echo "loader: e2fsck needs $so, not copied" >&2; exit 1; }
	done
	install -m 0755 "$HERE/s30-loader" "$loader/init"
	# USB disk mode and the no-card screen (see s30-loader; usb.bmp and
	# nocard.bmp from tools/make_loader_screens.py)
	[ -f "$WORK/s30-splash.bin" ] &&
		install -m 0755 "$WORK/s30-splash.bin" "$loader/bin/s30-splash"
	[ -f "$WORK/s30-keyheld.bin" ] &&
		install -m 0755 "$WORK/s30-keyheld.bin" "$loader/bin/s30-keyheld"
	install -m 0644 "$HERE/splash/usb.bmp" "$loader/usb.bmp"
	install -m 0644 "$HERE/splash/nocard.bmp" "$loader/nocard.bmp"
	fakeroot sh -c 'mknod -m 600 "$1/dev/console" c 5 1 &&
		cd "$1" && find . | cpio -o -H newc --quiet | gzip -9 > "$2"' \
		_ "$loader" "$WORK/initramfs.cpio.gz"
	"$WORK/u-boot/tools/mkimage" -A arm -O linux -T ramdisk -C none \
		-n s30-initramfs -d "$WORK/initramfs.cpio.gz" "$WORK/uInitrd"
	"$WORK/u-boot/tools/mkimage" -A arm -O linux -T script -C none \
		-d "$HERE/fel.cmd" "$WORK/fel.scr"
	"$WORK/u-boot/tools/mkimage" -A arm -O linux -T script -C none \
		-d "$HERE/fel-ram.cmd" "$WORK/fel-ram.scr"

	# The boot files (s30-linux, s30-uboot) and the board files (s30-base,
	# whose trigger installs the boot files on the card: boot-update)
	d="$pkg/s30-linux/usr/share/s30/boot"
	install -D -m 0644 -t "$d" "$WORK/boot/zImage" "$WORK/boot/sun8i-a33-pocketgo-s30.dtb" "$WORK/uInitrd"
	install -m 0644 "$WORK/fel.scr" "$d/boot.scr"
	mkpkg s30-linux "$(make -s -C "$WORK/linux" kernelversion)-r$PKG_REL" "$pkg/s30-linux"
	install -D -m 0644 -t "$pkg/s30-uboot/usr/share/s30/boot" "$WORK/u-boot-sunxi-with-spl.bin"
	mkpkg s30-uboot "$(make -s -C "$WORK/u-boot" ubootversion | tr - _)-r$PKG_REL" "$pkg/s30-uboot"
	cp -r "$HERE/rootfs-overlay" "$pkg/s30-base"
	# replaces: /etc/inittab is alpine-baselayout-data's, /etc/udhcpd.conf
	# busybox-extras'; ours stay through their upgrades (replaces-priority)
	MKPKG_DEPS=" s30-tools s30-linux s30-uboot"
	mkpkg s30-base "0.$PKG_REL-r0" "$pkg/s30-base" --trigger /usr/share/s30/boot \
		--script "trigger:$HERE/rootfs-overlay/usr/libexec/s30/boot-update" \
		--info "replaces:alpine-baselayout-data busybox-extras" --info "replaces-priority:100"
	mkndx
	apk add s30-base
	unset -f apk

	# ssh login as root with the build user's public keys (override with
	# S30_SSH_KEYS="file ..."; none found = no key login)
	mkdir -p -m 700 "$root/root/.ssh"
	cat ${S30_SSH_KEYS:-"$HOME"/.ssh/id_*.pub} > "$root/root/.ssh/authorized_keys" 2>/dev/null || true
	chmod 600 "$root/root/.ssh/authorized_keys"
	rm -f "$WORK/rootfs.sqfs" "$WORK/rootfs.ext4"
	mksquashfs "$root" "$WORK/rootfs.sqfs" -all-root -comp zstd -quiet -no-progress
	# Sparse on the host (full size once copied to the card). fakeroot: the
	# files owned by root in the image. No reserved blocks (root is the only
	# user); the inode tables and journal written now, not by the kernel in
	# the background at the first boot
	fakeroot mkfs.ext4 -q -L s30root -m 0 -E lazy_itable_init=0,lazy_journal_init=0 \
		-d "$root" "$WORK/rootfs.ext4" "${ROOTFS_EXT4_MB}M"
	ls -l "$WORK/rootfs.sqfs" "$WORK/rootfs.ext4" "$WORK/uInitrd" "$WORK"/repo/armv7/*.apk
}

# Boot over USB without installing anything. With the device on stock
# firmware (adb, see NOTES.md), the kernel, DTB and initramfs are copied to
# /s30 on the SD card over adb (fast), the stock firmware reboots into FEL, and
# FEL only uploads U-Boot and fel.scr. With the device already in FEL (e.g.
# from Linux: s30-fel), everything goes over FEL instead (slow, ~1 min).
# Addresses are U-Boot's sunxi defaults (kernel_addr_r, fdt_addr_r,
# scriptaddr, ramdisk_addr_r).
step_fel() {
	local i
	if adb get-state >/dev/null 2>&1; then
		adb shell mkdir /mnt/extsd/boot 2>/dev/null || true
		adb push --sync "$WORK/boot/zImage" "$WORK/boot/sun8i-a33-pocketgo-s30.dtb" \
			"$WORK/uInitrd" "$WORK/rootfs.sqfs" "$HERE/splash/logo.bmp" /mnt/extsd/boot/
		adb shell sync
		# One-shot boot0 FEL flag, then reboot
		adb shell '/bin/io -4 0x01f00108 0x5aa5a55a; /bin/reboot' >/dev/null 2>&1 || true
		for i in $(seq 30); do
			sunxi-fel ver >/dev/null 2>&1 && break
			sleep 1
		done
		sunxi-fel -p uboot "$WORK/u-boot-sunxi-with-spl.bin" \
			write 0x43100000 "$WORK/fel.scr"
	else
		echo "No adb device: uploading over FEL (uses /boot/rootfs.sqfs already on the SD card)"
		sunxi-fel -p uboot "$WORK/u-boot-sunxi-with-spl.bin" \
			write 0x42000000 "$WORK/boot/zImage" \
			write 0x43000000 "$WORK/boot/sun8i-a33-pocketgo-s30.dtb" \
			write 0x43100000 "$WORK/fel-ram.scr" \
			write 0x43400000 "$WORK/uInitrd"
	fi
}

# SPI flash image (8 MiB Zbit ZB25VQ64), written over /dev/mtd0 from Linux.
# Layout:
#   0x000000  U-Boot with SPL (BROM loads the SPL; U-Boot proper at 0x8000)
#   0x0c0000  FIT: zImage, DTB, initramfs (s30-loader), boot script
#             (flash.its, flash.cmd); U-Boot reads it when spi_booted is set
#   0x780000  Boot logo, uncompressed BMP (U-Boot patch 0007)
# The system stays on the SD card: flash.cmd boots /boot from the card when
# it loads (what the packages update) and the FIT's kernel otherwise. The
# BROM tries the card first: a card with U-Boot at 8 KiB still boots from the
# card. Installed by hand with s30-flash write (a bootloader update, not
# every kernel). Run after linux, uboot and initramfs.
FLASH_SIZE=$((0x800000))
FLASH_FIT_OFFS=$((0xc0000))
FLASH_LOGO_OFFS=$((0x780000))
build_fit() { # $WORK/flash.itb, checked against the layout
	local fit="$WORK/flash.itb" n
	n=$(stat -c %s "$WORK/u-boot-sunxi-with-spl.bin")
	[ "$n" -le "$FLASH_FIT_OFFS" ] ||
		{ echo "U-Boot is $n bytes, over the $FLASH_FIT_OFFS before the FIT" >&2; exit 1; }
	# dtc resolves /incbin/ paths from the .its file's directory
	cp "$HERE/flash.cmd" "$HERE/flash.its" "$WORK/"
	"$WORK/u-boot/tools/mkimage" -f "$WORK/flash.its" "$fit" > /dev/null
	n=$(stat -c %s "$fit")
	[ "$n" -le $((FLASH_LOGO_OFFS - FLASH_FIT_OFFS)) ] ||
		{ echo "FIT is $n bytes, over the $((FLASH_LOGO_OFFS - FLASH_FIT_OFFS)) before the logo" >&2; exit 1; }
	[ "$(stat -c %s "$HERE/splash/logo.bmp")" -le $((FLASH_SIZE - FLASH_LOGO_OFFS)) ] ||
		{ echo "logo.bmp does not fit after $FLASH_LOGO_OFFS" >&2; exit 1; }
}
step_flash() {
	local img="$OUT/spi-nor.bin" uboot="$WORK/u-boot-sunxi-with-spl.bin" fit="$WORK/flash.itb"
	build_fit
	mkdir -p "$OUT"
	# Erased flash reads 0xff
	head -c "$FLASH_SIZE" /dev/zero | tr '\0' '\377' > "$img"
	dd if="$uboot" of="$img" conv=notrunc status=none
	dd if="$fit" of="$img" bs=1 seek="$FLASH_FIT_OFFS" conv=notrunc status=none
	dd if="$HERE/splash/logo.bmp" of="$img" bs=1 seek="$FLASH_LOGO_OFFS" conv=notrunc status=none
	echo "Wrote $img (U-Boot $(stat -c %s "$uboot"), FIT $(stat -c %s "$fit") of $((FLASH_LOGO_OFFS - FLASH_FIT_OFFS)) bytes)"
}

# The package repository ($WORK/repo, build.sh initramfs) online: the
# gh-pages branch of origin, served by GitHub Pages at S30_APK_REPO, where
# the device's apk update looks. The branch is one commit holding only the
# current packages (replaced each time: a history of 30 MB publishes would
# pile up). Pages is switched on for the repository the first time (gh;
# a token without that right, GitHub Actions', only says so). Run by hand
# or by .github/workflows/packages.yml on every push to main.
step_publish() {
	local repo="$WORK/repo" gh="$WORK/gh-pages" desc slug
	[ -f "$repo/armv7/APKINDEX.tar.gz" ] || { echo "no $repo/armv7/APKINDEX.tar.gz: run build.sh initramfs first" >&2; exit 1; }
	# The versions come from the commit count: two builds of one commit
	# would have the same version, and the devices would never see the second
	[ -z "$(git -C "$PROJECT" status --porcelain --untracked-files=no --ignore-submodules)" ] ||
		{ echo "uncommitted changes: commit, build.sh initramfs, then publish" >&2; exit 1; }
	desc="$(git -C "$PROJECT" describe --always)"
	git -C "$PROJECT" worktree remove --force "$gh" 2>/dev/null || rm -rf "$gh"
	git -C "$PROJECT" worktree prune
	git -C "$PROJECT" branch -qD gh-pages 2>/dev/null || true
	git -C "$PROJECT" worktree add -q --detach "$gh" HEAD
	git -C "$gh" checkout -q --orphan gh-pages
	git -C "$gh" rm -rfq . 2>/dev/null || true
	rm -rf "$gh"/* 2>/dev/null || true
	mkdir -p "$gh/apk"
	cp -r "$repo/." "$gh/apk/"
	touch "$gh/.nojekyll"
	git -C "$gh" add -A
	git -C "$gh" -c user.name="build.sh publish" -c user.email="build@pocketgo-s30" \
		commit -qm "Packages $desc"
	git -C "$gh" push -qf origin gh-pages
	git -C "$PROJECT" worktree remove --force "$gh"
	git -C "$PROJECT" branch -qD gh-pages
	slug="$(git -C "$PROJECT" remote get-url origin | sed -E 's#.*github\.com[:/]##; s#\.git$##')"
	gh api "repos/$slug/pages" > /dev/null 2>&1 ||
		gh api -X POST "repos/$slug/pages" -f 'source[branch]=gh-pages' -f 'source[path]=/' > /dev/null ||
		echo "could not switch GitHub Pages on: repository Settings > Pages, deploy from the gh-pages branch" >&2
	echo "Published $(ls "$repo"/armv7/*.apk | wc -l) packages ($desc) to $S30_APK_REPO (Pages takes a minute)"
}

# Make an SD card boot on its own (no stock firmware, no FEL): the A33 boot
# ROM tries the card before SPI flash. Writes U-Boot into the gap before the
# first partition and the boot files onto that partition (exFAT, FAT or
# ext4, left as is). With "format", the card is first ERASED and given one
# exFAT partition at 1 MiB.
# Needs root; run after initramfs.
# Usage: sudo build.sh sdcard /dev/sdX [format]
step_sdcard() {
	local dev="${1:?usage: build.sh sdcard /dev/sdX [format]}"
	local name part start mnt tmp=
	name="$(basename "$dev")"
	[ -b "$dev" ] && [ "$(cat "/sys/block/$name/removable")" = 1 ] ||
		{ echo "$dev is not a removable disk" >&2; exit 1; }
	if [ "${2:-}" = format ]; then
		lsblk -lnpo NAME "$dev" | tail -n +2 | xargs -r umount 2>/dev/null || true
		wipefs -qa "$dev"
		printf 'label: dos\nstart=1MiB, type=7\n' | sfdisk -q "$dev"
		partprobe "$dev" 2>/dev/null || blockdev --rereadpt "$dev"
		udevadm settle
		part="$(lsblk -lnpo NAME,TYPE "$dev" | awk '$2 == "part" { print $1; exit }')"
		# The new partition starts where the old one did: clear its old
		# filesystem signature too, or mkfs refuses
		wipefs -qaf "$part" # -f: a FAT boot sector looks like a nested partition table, which wipefs skips otherwise
		mkfs.exfat -L S30 "$part"
	fi
	part="$(lsblk -lnpo NAME,TYPE "$dev" | awk '$2 == "part" { print $1; exit }')"
	[ -n "$part" ] || { echo "$dev has no partition" >&2; exit 1; }
	# U-Boot (SPL at 8 KiB, ~500 KiB) must end before the partition
	start="$(cat "/sys/block/$name/$(basename "$part")/start")"
	[ "$start" -ge 2048 ] ||
		{ echo "$part starts at sector $start, needs >= 2048 (1 MiB)" >&2; exit 1; }

	dd if="$WORK/u-boot-sunxi-with-spl.bin" of="$dev" bs=1k seek=8 \
		conv=notrunc,fsync status=none

	# findmnt exits 1 when the partition is not mounted (pipefail)
	mnt="$(findmnt -nro TARGET -S "$part" | head -n1 || true)"
	if [ -z "$mnt" ]; then
		tmp="$(mktemp -d)"; mnt="$tmp"
		mount "$part" "$mnt"
	fi
	# /boot: system images. /home: $HOME on the device (RetroArch settings in
	# ~/.config/retroarch, saves), created on first boot; ROMs in /mnt/sd/roms,
	# the ports in games/, launch scripts for other apps in apps/.
	mkdir -p "$mnt/boot" "$mnt/home" "$mnt/roms" "$mnt/games" "$mnt/apps"
	# Writable from the PC by whoever ran sudo (the device runs as root)
	# (ext4 only: exFAT/FAT have no owners and refuse chown)
	[ -z "${SUDO_UID:-}" ] || chown -R "$SUDO_UID:$SUDO_GID" "$mnt/home" 2>/dev/null || true
	cp "$WORK/boot/zImage" "$WORK/boot/sun8i-a33-pocketgo-s30.dtb" \
		"$WORK/uInitrd" "$WORK/rootfs.sqfs" "$HERE/splash/logo.bmp" "$mnt/boot/"
	# The read-write root (preferred by the loader when present); a card
	# without it boots rootfs.sqfs. Copied in full (1 GiB): exFAT has no
	# sparse files
	[ ! -f "$WORK/rootfs.ext4" ] || cp "$WORK/rootfs.ext4" "$mnt/boot/"
	# U-Boot's default bootcmd (distro boot) runs /boot/boot.scr; same
	# script as FEL
	cp "$WORK/fel.scr" "$mnt/boot/boot.scr"
	# Earlier layout (/s30, /boot.scr, empty /retroarch folders)
	rm -rf "$mnt/s30" "$mnt/boot.scr"
	rmdir "$mnt"/retroarch/{roms,saves,states,system,screenshots} \
		"$mnt/retroarch" 2>/dev/null || true
	sync
	if [ -n "$tmp" ]; then umount "$mnt"; rmdir "$tmp"; fi
	echo "SD card $dev ready (boot files on $part)"
}

case "${1:-all}" in
	deps) step_deps ;;
	linux) step_linux ;;
	uboot) step_uboot ;;
	rootfs) step_rootfs ;;
	initramfs) step_initramfs ;;
	retroarch) step_retroarch ;;
	emulationstation) step_emulationstation ;;
	launcher) step_launcher ;;
	mesa) step_mesa ;;
	gmloader) step_gmloader ;;
	fel) step_fel ;;
	sdcard) step_sdcard "${2:-}" "${3:-}" ;;
	flash) step_flash ;;
	publish) step_publish ;;
	all) step_deps; step_linux; step_uboot; step_rootfs; step_initramfs ;;
	*) echo "usage: $0 [deps|linux|uboot|rootfs|initramfs|retroarch|emulationstation|launcher|mesa|gmloader|fel|sdcard /dev/sdX [format]|flash|publish|all]" >&2; exit 2 ;;
esac
