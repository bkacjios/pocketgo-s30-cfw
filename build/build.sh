#!/bin/bash
# Build a bootable PocketGo S30 SD card image (mainline U-Boot + Linux + Alpine).
#
# Run on a Debian/Ubuntu or Arch host:  bash build/build.sh
#
# Linux, U-Boot and RetroArch are git submodules (external/, pinned
# versions); each is built in a git worktree under $WORK (Linux filesystem;
# the kernel tree cannot be checked out on NTFS), so board patches never touch
# the submodule checkout. The result is written to <project>/out/sdcard.img.
#
# Steps can be run individually: build.sh deps|linux|uboot|rootfs|image
# USB boot from stock firmware (no flashing): build.sh initramfs, then fel
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(dirname "$HERE")"
WORK="${WORK:-$PROJECT/s30-build}"
OUT="${OUT:-$PROJECT/out}"
JOBS="$(nproc)"
export ARCH=arm

ALPINE_MIRROR=https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/armv7
ALPINE_REPO=https://dl-cdn.alpinelinux.org/alpine/latest-stable

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
			clang lld llvm squashfs-tools pkgconf
		return
	fi
	sudo apt-get update
	sudo apt-get install -y --no-install-recommends \
		git make gcc bc bison flex libssl-dev libelf-dev libncurses-dev \
		gcc-arm-linux-gnueabihf device-tree-compiler swig python3-dev \
		python3-setuptools python3-pyelftools libgnutls28-dev uuid-dev \
		e2fsprogs fdisk fakeroot curl ca-certificates cpio kmod \
		clang lld llvm squashfs-tools pkg-config
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
	# Extraction and image creation share one fakeroot session so files
	# keep root ownership inside the ext4 image.
	echo "$tarball" > "$WORK/rootfs.src"

	# Extra packages from build/packages plus their dependencies (resolved
	# from the repo indexes; packages already in minirootfs are skipped)
	local repo idx name ver
	mkdir -p "$WORK/apk"
	for repo in main community; do
		curl -fsSL "$ALPINE_REPO/$repo/armv7/APKINDEX.tar.gz" |
			tar -xzO APKINDEX > "$WORK/apk/APKINDEX.$repo"
	done
	tar -xzOf "$tarball" ./lib/apk/db/installed > "$WORK/apk/installed"
	python3 -I "$PROJECT/tools/apk_resolve.py" "$WORK/apk/installed" \
		"$WORK/apk/APKINDEX.main" "$WORK/apk/APKINDEX.community" \
		-- $(grep -v '^#' "$HERE/packages") > "$WORK/apk/list"
	rm -f "$WORK/apk/"*.apk
	while read -r idx name ver; do
		repo="${idx##*APKINDEX.}"
		curl -fsSL -o "$WORK/apk/$name-$ver.apk" "$ALPINE_REPO/$repo/armv7/$name-$ver.apk"
	done < "$WORK/apk/list"
	echo "Packages: $(wc -l < "$WORK/apk/list") ($(cut -d' ' -f2 "$WORK/apk/list" | tr '\n' ' '))"
}

# Cross sysroot for clang builds (RetroArch, launcher): Alpine minirootfs +
# toolchain runtime (gcc for crt/libgcc) + -dev packages, and single-word
# compiler wrappers $WORK/bin/armv7-clang[++]. Rebuilt when the list changes.
SYSROOT_PKGS="gcc g++ musl-dev linux-headers libdrm-dev mesa-dev alsa-lib-dev eudev-dev zlib-dev sdl2-dev sdl2_ttf-dev
	freeimage-dev freetype-dev curl-dev vlc-dev rapidjson-dev pugixml-dev"
ensure_sysroot() {
	local sr="$WORK/sysroot" idx name ver repo t
	if [ "$(cat "$sr/.done" 2>/dev/null)" != "$SYSROOT_PKGS" ]; then
		rm -rf "$sr" "$WORK/sysroot-apk"; mkdir -p "$sr" "$WORK/sysroot-apk"
		tar -xzf "$(cat "$WORK/rootfs.src")" -C "$sr"
		# shellcheck disable=SC2086
		python3 -I "$PROJECT/tools/apk_resolve.py" "$WORK/apk/installed" \
			"$WORK/apk/APKINDEX.main" "$WORK/apk/APKINDEX.community" -- \
			$SYSROOT_PKGS > "$WORK/sysroot-apk/list"
		while read -r idx name ver; do
			repo="${idx##*APKINDEX.}"
			curl -fsSL -o "$WORK/sysroot-apk/$name-$ver.apk" \
				"$ALPINE_REPO/$repo/armv7/$name-$ver.apk"
			tar -xzf "$WORK/sysroot-apk/$name-$ver.apk" -C "$sr" --exclude=".*" 2>/dev/null
		done < "$WORK/sysroot-apk/list"
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
# sound card). Output: $WORK/{launcher,s30-*}.bin, $WORK/libs30lease.so
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
	"$WORK/bin/armv7-clang" -O2 -Wall -fPIC -shared -o "$WORK/libs30lease.so" "$PROJECT/utils/s30-lease.c"
	llvm-strip "$WORK/s30-fel.bin" "$WORK/s30-volumed.bin" "$WORK/s30-splash.bin" \
		"$WORK/s30-keyheld.bin" "$WORK/s30-display.bin" "$WORK/s30-screenshot.bin" "$WORK/s30-uvc.bin" "$WORK/s30-uac.bin" \
		"$WORK/libs30lease.so"
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

# Root filesystem for build.sh fel: rootfs.sqfs (minirootfs + packages +
# rootfs-overlay) goes to /s30 on the SD card; uInitrd is a small loader (busybox,
# musl, s30-loader as /init) that mounts it with a tmpfs overlay.
step_initramfs() {
	local root="$WORK/rootfs" loader="$WORK/loader" apk
	rm -rf "$root" "$loader"; mkdir -p "$root" "$loader"
	tar -xzf "$(cat "$WORK/rootfs.src")" -C "$root"
	for apk in "$WORK"/apk/*.apk; do
		[ -e "$apk" ] && tar -xzf "$apk" -C "$root" --exclude=".*" 2>/dev/null
	done
	# Board files: init setup (/etc/inittab, /etc/s30/rc), configs, scripts
	# (build/rootfs-overlay mirrors the rootfs layout)
	cp -r "$HERE/rootfs-overlay/." "$root/"
	# ssh login as root with the build user's public keys (override with
	# S30_SSH_KEYS="file ..."; none found = no key login)
	mkdir -p -m 700 "$root/root/.ssh"
	cat ${S30_SSH_KEYS:-"$HOME"/.ssh/id_*.pub} > "$root/root/.ssh/authorized_keys" 2>/dev/null || true
	chmod 600 "$root/root/.ssh/authorized_keys"
	[ -f "$WORK/retroarch.bin" ] &&
		install -m 0755 "$WORK/retroarch.bin" "$root/usr/bin/retroarch"
	[ -f "$WORK/launcher.bin" ] &&
		install -m 0755 "$WORK/launcher.bin" "$root/usr/bin/s30-launcher"
	[ -f "$WORK/s30-fel.bin" ] &&
		install -m 0755 "$WORK/s30-fel.bin" "$root/usr/bin/s30-fel"
	[ -f "$WORK/s30-flash.bin" ] &&
		install -m 0755 "$WORK/s30-flash.bin" "$root/usr/bin/s30-flash"
	[ -f "$WORK/s30-volumed.bin" ] &&
		install -m 0755 "$WORK/s30-volumed.bin" "$root/usr/bin/s30-volumed"
	[ -f "$WORK/s30-splash.bin" ] &&
		install -m 0755 "$WORK/s30-splash.bin" "$root/usr/bin/s30-splash"
	[ -f "$WORK/s30-display.bin" ] &&
		install -m 0755 "$WORK/s30-display.bin" "$root/usr/bin/s30-display"
	[ -f "$WORK/s30-screenshot.bin" ] &&
		install -m 0755 "$WORK/s30-screenshot.bin" "$root/usr/bin/s30-screenshot"
	[ -f "$WORK/s30-uvc.bin" ] &&
		install -m 0755 "$WORK/s30-uvc.bin" "$root/usr/bin/s30-uvc"
	[ -f "$WORK/s30-uac.bin" ] &&
		install -m 0755 "$WORK/s30-uac.bin" "$root/usr/bin/s30-uac"
	[ -d "$WORK/emulationstation-root" ] &&
		cp -r "$WORK/emulationstation-root/." "$root/"
	# Mesa (build.sh mesa), in place of Alpine's (build/packages excludes it)
	[ -d "$WORK/mesa-root" ] &&
		cp -r "$WORK/mesa-root/." "$root/"
	# EmulationStation theme (submodule external/es-theme-carbon)
	if [ -f "$PROJECT/external/es-theme-carbon/carbon.xml" ]; then
		mkdir -p "$root/etc/emulationstation/themes/carbon"
		tar -C "$PROJECT/external/es-theme-carbon" -cf - --exclude=.git . |
			tar -C "$root/etc/emulationstation/themes/carbon" -xf -
	fi
	[ -f "$WORK/libs30lease.so" ] &&
		install -D -m 0755 "$WORK/libs30lease.so" "$root/usr/lib/s30/libs30lease.so"
	# Boot splash: the same image U-Boot shows
	install -D -m 0644 "$HERE/splash/logo.bmp" "$root/usr/share/s30/splash.bmp"
	rm -f "$WORK/rootfs.sqfs"
	mksquashfs "$root" "$WORK/rootfs.sqfs" -all-root -comp zstd -quiet -no-progress

	mkdir -p "$loader"/{bin,lib,dev,proc,sys}
	cp -a "$root/bin/busybox" "$loader/bin/"
	cp -a "$root"/lib/ld-musl-armhf.so.1 "$loader/lib/"
	# Card check (dosfstools, exfatprogs; libc only)
	cp -a "$root/sbin/fsck.fat" "$root/usr/sbin/fsck.exfat" "$loader/bin/"
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
	ls -l "$WORK/rootfs.sqfs" "$WORK/uInitrd"
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

step_image() {
	local img="$OUT/sdcard.img" part="$WORK/rootfs.ext4"
	local size_mb=512 offset_mb=2
	rm -f "$part"
	fakeroot bash -euo pipefail -c '
		root="$1/rootfs"; rm -rf "$root"; mkdir -p "$root"
		tar -xzf "$(cat "$1/rootfs.src")" -C "$root"
		for apk in "$1"/apk/*.apk; do
			[ -e "$apk" ] && tar -xzf "$apk" -C "$root" --exclude=".*" 2>/dev/null
		done
		cp -r "$2/rootfs-overlay/." "$root/"
		mkdir -p "$root/boot"
		cp "$1"/boot/* "$root/boot/"
		mkfs.ext4 -q -L s30root -d "$root" "$3" "$4M"
	' _ "$WORK" "$HERE" "$part" "$((size_mb - offset_mb))"

	rm -f "$img"
	truncate -s "${size_mb}M" "$img"
	printf 'label: dos\nstart=%dMiB, type=83, bootable\n' "$offset_mb" | sfdisk -q "$img"
	# A33 boot ROM loads SPL from 8 KiB into the card.
	dd if="$WORK/u-boot-sunxi-with-spl.bin" of="$img" bs=1k seek=8 conv=notrunc status=none
	dd if="$part" of="$img" bs=1M seek="$offset_mb" conv=notrunc status=none
	echo "Wrote $img"
}

# SPI flash image (8 MiB Zbit ZB25VQ64), written over /dev/mtd0 from Linux.
# Layout:
#   0x000000  U-Boot with SPL (BROM loads the SPL; U-Boot proper at 0x8000)
#   0x0c0000  FIT: zImage, DTB, initramfs (s30-loader), boot script
#             (flash.its, flash.cmd); U-Boot reads it when spi_booted is set
#   0x780000  Boot logo, uncompressed BMP (U-Boot patch 0007)
# The root filesystem (rootfs.sqfs) and user data stay on the SD card. The
# BROM tries the card first: a card with U-Boot at 8 KiB still boots from the
# card. Install on the device with s30-flash write. Run after linux, uboot
# and initramfs.
FLASH_SIZE=$((0x800000))
FLASH_FIT_OFFS=$((0xc0000))
FLASH_LOGO_OFFS=$((0x780000))
step_flash() {
	local img="$OUT/spi-nor.bin" uboot="$WORK/u-boot-sunxi-with-spl.bin"
	local fit="$WORK/flash.itb" n
	n=$(stat -c %s "$uboot")
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
	mkdir -p "$OUT"
	# Erased flash reads 0xff
	head -c "$FLASH_SIZE" /dev/zero | tr '\0' '\377' > "$img"
	dd if="$uboot" of="$img" conv=notrunc status=none
	dd if="$fit" of="$img" bs=1 seek="$FLASH_FIT_OFFS" conv=notrunc status=none
	dd if="$HERE/splash/logo.bmp" of="$img" bs=1 seek="$FLASH_LOGO_OFFS" conv=notrunc status=none
	echo "Wrote $img (U-Boot $(stat -c %s "$uboot"), FIT $n of $((FLASH_LOGO_OFFS - FLASH_FIT_OFFS)) bytes)"
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
		wipefs -qa "$part"
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
	# ~/.config/retroarch, saves, ROMs in ~/roms), created on first boot.
	mkdir -p "$mnt/boot" "$mnt/home/roms"
	# Writable from the PC by whoever ran sudo (the device runs as root)
	# (ext4 only: exFAT/FAT have no owners and refuse chown)
	[ -z "${SUDO_UID:-}" ] || chown -R "$SUDO_UID:$SUDO_GID" "$mnt/home" 2>/dev/null || true
	cp "$WORK/boot/zImage" "$WORK/boot/sun8i-a33-pocketgo-s30.dtb" \
		"$WORK/uInitrd" "$WORK/rootfs.sqfs" "$HERE/splash/logo.bmp" "$mnt/boot/"
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
	image) step_image ;;
	initramfs) step_initramfs ;;
	retroarch) step_retroarch ;;
	emulationstation) step_emulationstation ;;
	launcher) step_launcher ;;
	mesa) step_mesa ;;
	gmloader) step_gmloader ;;
	fel) step_fel ;;
	sdcard) step_sdcard "${2:-}" "${3:-}" ;;
	flash) step_flash ;;
	all) step_deps; step_linux; step_uboot; step_rootfs; step_image ;;
	*) echo "usage: $0 [deps|linux|uboot|rootfs|image|initramfs|retroarch|emulationstation|launcher|mesa|gmloader|fel|sdcard /dev/sdX [format]|flash|all]" >&2; exit 2 ;;
esac
