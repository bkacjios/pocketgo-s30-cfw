# Boot script for build.sh fel. FEL (USB) only uploads U-Boot and this
# script, because FEL transfers are slow; build.sh fel copies the kernel,
# DTB and initramfs to /s30 on the SD card over adb beforehand.
setenv bootargs console=tty1 console=ttyS0,115200 quiet loglevel=3 panel_ilitek_ili9488_dsi.handoff=1 vt.global_cursor_default=0 fbcon=logo-pos:center
load mmc 0:1 ${kernel_addr_r} /boot/zImage
load mmc 0:1 ${fdt_addr_r} /boot/sun8i-a33-pocketgo-s30.dtb
load mmc 0:1 ${ramdisk_addr_r} /boot/uInitrd
bootz ${kernel_addr_r} ${ramdisk_addr_r} ${fdt_addr_r}
