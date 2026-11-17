#!/bin/sh
# s30-console
# Alpine package updates (apk update, apk upgrade) on the device's root
# (/boot/rootfs.ext4 on the card, read-write, so they stay). Needs internet
# through the USB network: the PC shares its connection (NOTES.md, "USB
# network"). The clock is set from NTP first (the package mirror's
# certificates need a sane date); the output is on the LCD (s30-console).
. /etc/profile.d/s30.sh

finish() { # STATUS [reboot]: START (315) exits; with reboot, A (305) reboots instead
	echo
	if [ "${2:-}" = reboot ]; then
		echo "Press A to reboot now, START to exit."
		if [ "$(s30-keyheld -w 315 305)" = 305 ]; then
			echo "Rebooting..."
			sync
			reboot
			sleep 10
		fi
	else
		echo "Press START to exit."
		s30-keyheld -w 315 > /dev/null
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
# Several servers: busybox ntpd resolves each -p to one address, takes the
# first that answers, and gives up at 10 s when one server stays silent
# (some pool members never answer; with one -p that failed every few runs)
if timeout 20 ntpd -q -n -p 0.pool.ntp.org -p 1.pool.ntp.org -p 2.pool.ntp.org \
		-p time.cloudflare.com >/dev/null 2>&1; then
	hwclock -w 2>/dev/null
	date
else
	echo "No answer from the time servers, keeping $(date)"
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
