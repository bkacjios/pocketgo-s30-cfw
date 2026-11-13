# Boot script inside the SPI flash FIT (build.sh flash), run by U-Boot's
# bootcmd after it read the FIT to ${fit_addr}. Same kernel command line as
# fel.cmd; kernel, DTB and initramfs all come from the FIT.
setenv bootargs console=tty1 console=ttyS0,115200 quiet loglevel=3 panel_ilitek_ili9488_dsi.handoff=1 vt.global_cursor_default=0 fbcon=logo-pos:center
bootm ${fit_addr}#s30
