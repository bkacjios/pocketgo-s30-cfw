setenv bootargs console=tty1 console=ttyS0,115200 root=/dev/mmcblk0p1 rootwait rw
load mmc 0:1 ${kernel_addr_r} /boot/zImage
load mmc 0:1 ${fdt_addr_r} /boot/sun8i-a33-pocketgo-s30.dtb
bootz ${kernel_addr_r} - ${fdt_addr_r}
