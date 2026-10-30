#!/bin/bash
# Dump AXP223 registers (over RSB) and the CCU from a device sitting in FEL
# after SPL has run (SPL brings up RSB at runtime address 0x2d).
# Usage: fel_axp_dump.sh > dump.txt
set -u
RSB=0x01f03400
fel() { timeout 5 sunxi-fel "$@"; }
rd() { fel readl $1; }
wr() { fel writel $1 $2; }
for r in $(seq 0 255); do
	wr $((RSB + 0x2c)) 0x8b                  # CMD: read 8 bit
	wr $((RSB + 0x30)) $((0x2d << 16))       # DAR: runtime address
	wr $((RSB + 0x10)) $r                    # ADDR
	wr $((RSB + 0x00)) 0x80                  # CTRL: start transfer
	for i in 1 2 3 4 5; do
		[ $(( $(rd $((RSB + 0x00))) & 0x80 )) -eq 0 ] && break
	done
	st=$(rd $((RSB + 0x0c))); wr $((RSB + 0x0c)) $st
	printf 'axp %02x = %02x (stat %s)\n' $r $(( $(rd $((RSB + 0x1c))) & 0xff )) $st
done
fel hexdump 0x01c20000 0x400 | sed 's/^/ccu /'
fel hexdump 0x01f01400 0x200 | sed 's/^/prcm /'
