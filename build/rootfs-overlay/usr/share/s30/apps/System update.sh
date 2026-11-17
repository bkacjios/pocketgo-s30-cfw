#!/bin/sh
# s30-console
# Alpine package updates (apk update, apk upgrade) on the device's root
# (/boot/rootfs.ext4 on the card, read-write, so they stay). Needs internet
# through the USB network: the PC shares its connection (NOTES.md, "USB
# network"). The clock is set from NTP first (the package mirror's
# certificates need a sane date); the output is on the LCD (s30-console).
. /etc/profile.d/s30.sh

finish() { # STATUS [reboot]: START exits; with reboot, A (BTN_EAST 305) reboots first
	echo
	if [ "${2:-}" = reboot ]; then
		echo "Press A to reboot now, START to exit."
		until s30-keyheld 315; do
			s30-keyheld 305 && { echo "Rebooting..."; sync; reboot; sleep 10; }
			sleep 0.2
		done
	else
		echo "Press START to exit."
		until s30-keyheld 315; do sleep 0.2; done
	fi
	exit "$1"
}

echo "System update"
echo
if ! ip -4 route | grep -q '^default'; then
	echo "No internet connection."
	echo
	echo "The PC has to share its connection over USB"
	echo "(NetworkManager: Shared to other computers)"
	echo "and the device plugged in after that."
	finish 1
fi

echo "Setting the clock..."
if timeout 20 ntpd -q -n -p pool.ntp.org >/dev/null 2>&1; then
	hwclock -w 2>/dev/null
	date
else
	echo "No answer from pool.ntp.org, keeping $(date)"
fi
echo

echo "Fetching the package lists..."
if ! apk update 2>&1; then
	echo
	echo "The package mirror could not be reached."
	finish 1
fi
echo

echo "Upgrading..."
out=$(apk upgrade 2>&1); rc=$?
echo "$out"
if [ $rc != 0 ]; then
	echo
	echo "The upgrade failed (apk exit $rc)."
	finish 1
fi
# What changed, from apk's "(n/m) Upgrading name (old -> new)" lines, and
# when it takes effect: the kernel, loader and U-Boot (s30-linux, s30-uboot)
# and the system itself (s30-base: init and scripts, s30-tools: launcher
# and display, s30-mesa) at the next boot; apps and libraries when an app
# starts next
changed=$(echo "$out" | sed -n 's/^([0-9]*\/[0-9]*) [A-Za-z]* \([^ ]*\) .*/\1/p')
echo
if [ -z "$changed" ]; then
	echo "Everything is up to date."
	finish 0
fi
echo "Updated: $(echo "$changed" | tr '\n' ' ')"
echo
boot= system=
for p in $changed; do
	case $p in
	s30-linux|s30-uboot) boot=1 ;;
	s30-base|s30-tools|s30-mesa) system=1 ;;
	esac
done
if [ -n "$boot$system" ]; then
	[ -z "$boot" ] || echo "The new kernel and boot files run from the next boot."
	[ -z "$system" ] || echo "The new system parts (launcher, display, init) run from the next boot."
	finish 0 reboot
fi
echo "Apps and libraries: in use the next time an app starts."
finish 0
