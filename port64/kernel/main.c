/* DOORS kernel entry (M1: long-mode boot to a banner).
   Called from boot/boot.asm in 64-bit mode on the kernel stack, with paging on and the
   first 1 GiB mapped both at 0 and at KERNEL_VMA. */

#include <doors/console.h>
#include <doors/multiboot2.h>
#include <doors/x86.h>

#define DOORS_VERSION "0.1"

/* QEMU isa-debug-exit device (only present in `make test`): QEMU exits with (code << 1) | 1. */
#define QEMU_EXIT_PORT 0xF4
#define QEMU_EXIT_OK   0x10	/* -> exit status 33 */

extern U8 _kernel_start[], _kernel_end[];	/* linker.ld */

static void banner(void)
{
	vga_set_color(WHITE, BLUE);
	kprintf(" DOORS %s (x86-64)  Message-based multitasking real-time kernel           \n",
		DOORS_VERSION);
	vga_set_color(LIGHTGRAY, BLACK);
	kprintf("Based on MMURTL V1.0 by Richard A. Burgess (public domain)\n\n");
}

void kmain(U32 magic, U64 mbi_phys)
{
	struct boot_info bi;
	U64 efer;

	serial_init();
	vga_init();
	banner();

	if (magic != MB2_BOOTLOADER_MAGIC)
		panic("not booted by a Multiboot2 loader (magic %x)", magic);

	mb2_parse(mbi_phys, &bi, true);

	efer = rdmsr(MSR_EFER);
	kprintf("\nLoader:   %s\n", bi.loader);
	kprintf("Cmdline:  %s\n", bi.cmdline);
	kprintf("Memory:   %lu MB usable (%u map entries)\n", bi.mem_usable >> 20, bi.mmap_count);
	kprintf("Kernel:   %p - %p (%lu KB)\n", _kernel_start, _kernel_end,
		(U64)(_kernel_end - _kernel_start) >> 10);
	kprintf("CPU:      long mode %s, CR0=%lx CR3=%lx CR4=%lx EFER=%lx\n",
		(efer & (1 << 10)) ? "active" : "INACTIVE", read_cr0(), read_cr3(), read_cr4(), efer);

	if (!(efer & (1 << 10)))
		panic("EFER.LMA not set");

	kprintf("\nDOORS: M1 OK\n");

	if (cmdline_has(bi.cmdline, "test"))
		outb(QEMU_EXIT_PORT, QEMU_EXIT_OK);

	halt_forever();
}
