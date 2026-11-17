# Boot script inside the SPI flash FIT (build.sh flash), run by U-Boot's
# bootcmd after it read the FIT to ${fit_addr}. Same kernel command line as
# fel.cmd. The system is on the SD card: its /boot files (kept current by
# the s30-linux package and boot-update) are booted when they load; the
# kernel, DTB and loader in the FIT are the fallback for a card without them
# (or no card: the loader then waits for one).
setenv bootargs console=tty1 console=ttyS0,115200 quiet loglevel=3 panel_ilitek_ili9488_dsi.handoff=1 vt.global_cursor_default=0 fbcon=logo-pos:center
if load mmc 0:1 ${kernel_addr_r} /boot/zImage && load mmc 0:1 ${fdt_addr_r} /boot/sun8i-a33-pocketgo-s30.dtb && load mmc 0:1 ${ramdisk_addr_r} /boot/uInitrd; then
	bootz ${kernel_addr_r} ${ramdisk_addr_r} ${fdt_addr_r}
fi
bootm ${fit_addr}#s30
