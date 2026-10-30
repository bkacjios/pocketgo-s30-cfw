# Boot script for build.sh fel without adb: the kernel, DTB and initramfs
# were uploaded over FEL to U-Boot's default load addresses.
setenv bootargs console=ttyS0,115200 console=tty1 quiet vt.global_cursor_default=0 fbcon=logo-pos:center
bootz ${kernel_addr_r} ${ramdisk_addr_r} ${fdt_addr_r}
