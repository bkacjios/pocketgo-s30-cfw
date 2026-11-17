#!/bin/sh
# s30-console
# Alpine package updates (apk update, apk upgrade) on the device's root
# (/boot/rootfs.ext4 on the card, read-write, so they stay). Needs internet
# through the USB network: the PC shares its connection (NOTES.md, "USB
# network"). The clock is set from NTP first (the package mirror's
# certificates need a sane date); the output is on the LCD (s30-console).
. /etc/profile.d/s30.sh

finish() {
	echo
	echo "Press START to go back."
	until s30-keyheld 315; do sleep 0.2; done
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
n=$(echo "$out" | grep -c '^([0-9]*/[0-9]*) ')
echo
if [ "$n" = 0 ]; then
	echo "Everything is up to date."
else
	echo "$n packages changed. Reboot to run the new versions."
fi
finish 0
