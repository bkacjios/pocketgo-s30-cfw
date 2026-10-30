/*
 * s30-fel: reboot into FEL (USB boot) mode. The kernel's reboot mode "fel"
 * (nvmem-reboot-mode in the device tree) sets the one-shot flag boot0 checks.
 */
#include <linux/reboot.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
	sync();
	syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
		LINUX_REBOOT_CMD_RESTART2, "fel");
	perror("s30-fel: reboot");
	return 1;
}
