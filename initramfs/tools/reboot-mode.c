// reboot-mode <mode>: reboot(LINUX_REBOOT_CMD_RESTART2, mode), e.g. "bootloader".
// Freestanding (no libc) so it can be built with the kernel's clang for the initramfs.
#define SYS_sync 81
#define SYS_reboot 142
#define SYS_exit 93

static long sc(long n, long a, long b, long c, long d)
{
	register long x8 asm("x8") = n, x0 asm("x0") = a, x1 asm("x1") = b,
		      x2 asm("x2") = c, x3 asm("x3") = d;
	asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
}

void _start_c(long *sp)
{
	char **argv = (char **)(sp + 1);
	const char *mode = sp[0] > 1 ? argv[1] : "bootloader";

	sc(SYS_sync, 0, 0, 0, 0);
	sc(SYS_reboot, 0xfee1dead, 672274793, 0xa1b2c3d4 /* RESTART2 */, (long)mode);
	sc(SYS_exit, 1, 0, 0, 0);
}

asm(".globl _start\n_start:\n mov x0, sp\n b _start_c\n");
