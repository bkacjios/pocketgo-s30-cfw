# Boot script for build.sh fel without adb: the kernel, DTB and initramfs
# were uploaded over FEL to U-Boot's default load addresses. Same command
# line as fel.cmd (U-Boot initialized the panel: handoff=1).
setenv bootargs console=tty1 console=ttyS0,115200 quiet loglevel=3 panel_ilitek_ili9488_dsi.handoff=1 vt.global_cursor_default=0 fbcon=logo-pos:center
bootz ${kernel_addr_r} ${ramdisk_addr_r} ${fdt_addr_r}
