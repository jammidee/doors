/* DOORS kernel entry.
   Called from boot/boot.asm in 64-bit mode on the kernel stack, with paging on and the
   first 1 GiB mapped both at 0 and at KERNEL_VMA. */

#include <doors/console.h>
#include <doors/cpu.h>
#include <doors/kernel.h>
#include <doors/multiboot2.h>
#include <doors/string.h>
#include <doors/x86.h>

#define QEMU_EXIT_PORT 0xF4
#define TICK_HZ        100	/* 10 ms, as MMURTL */

bool test_mode;

extern U8 _kernel_start[], _kernel_end[];	/* linker.ld */

void qemu_exit(U8 code)
{
	outb(QEMU_EXIT_PORT, code);
}

static void banner(void)
{
	static const char title[] = " DOORS " DOORS_VERSION
		" (x86-64)  Message-based multitasking real-time kernel";

	vga_set_title(title);
	serial_write(title);
	serial_write("\n");
	kprintf("Based on MMURTL V1.0 by Richard A. Burgess (public domain)\n\n");
}

static void boot_info(U64 mbi_phys)
{
	struct boot_info bi;
	U64 efer = rdmsr(MSR_EFER);

	mb2_parse(mbi_phys, &bi, true);
	test_mode = cmdline_has(bi.cmdline, "test");

	kprintf("Loader:  %s   Cmdline: %s\n", bi.loader, bi.cmdline);
	kprintf("Memory:  %lu MB usable (%u map entries)\n", bi.mem_usable >> 20, bi.mmap_count);
	kprintf("Kernel:  %p - %p (%lu KB)\n", _kernel_start, _kernel_end,
		(U64)(_kernel_end - _kernel_start) >> 10);
	kprintf("CPU:     long mode %s, CR0=%lx CR3=%lx CR4=%lx EFER=%lx\n",
		(efer & (1 << 10)) ? "active" : "INACTIVE", read_cr0(), read_cr3(), read_cr4(), efer);
	if (!(efer & (1 << 10)))
		panic("EFER.LMA not set");
}

/* ---- interactive mode: a tiny line monitor to try the M2 features by hand ---- */

static void cmd(const char *line)
{
	if (line[0] == '\0')
		return;
	if (!strcmp(line, "help")) {
		kprintf("help   this list\n"
			"ticks  show the 10 ms tick counter\n"
			"int3   breakpoint exception (handled, execution continues)\n"
			"pf     page fault at 0xdead0000 (register dump + panic)\n"
			"div0   divide error (register dump + panic)\n");
	} else if (!strcmp(line, "ticks")) {
		kprintf("Tick: %lu (%lu.%02lu s since interrupts were enabled)\n",
			timer_ticks(), timer_ticks() / TICK_HZ, timer_ticks() % TICK_HZ);
	} else if (!strcmp(line, "int3")) {
		__asm__ volatile("int3");
		kprintf("back from the breakpoint handler\n");
	} else if (!strcmp(line, "pf")) {
		*(volatile U64 *)0xdead0000ULL = 1;
	} else if (!strcmp(line, "div0")) {
		volatile U32 zero = 0;
		kprintf("%u\n", 1 / zero);
	} else {
		kprintf("unknown command '%s' (try help)\n", line);
	}
}

static void breakpoint(struct regs *r)
{
	kprintf("Breakpoint at RIP=%016lx\n", r->rip);
}

static NORETURN void interactive(void)
{
	char line[64];
	U32 n = 0;

	isr_register(3, breakpoint);
	kprintf("Type 'help' for commands. Keys are echoed.\n> ");
	for (;;) {
		int c = kbd_getc();

		if (c < 0) {
			hlt();
			continue;
		}
		if (c == '\n') {
			console_putc('\n');
			line[n] = '\0';
			cmd(line);
			n = 0;
			kprintf("> ");
		} else if (c == '\b') {
			if (n) {
				n--;
				console_write("\b \b");
			}
		} else if (n < sizeof(line) - 1 && c >= ' ') {
			line[n++] = (char)c;
			console_putc((char)c);
		}
	}
}

void kmain(U32 magic, U64 mbi_phys)
{
	serial_init();
	vga_init();
	banner();

	if (magic != MB2_BOOTLOADER_MAGIC)
		panic("not booted by a Multiboot2 loader (magic %x)", magic);
	boot_info(mbi_phys);

	gdt_init();
	idt_init();
	pic_init();
	pit_init(TICK_HZ);
	kbd_init();
	sti();
	kprintf("Interrupts: GDT+TSS (IST for #DF/NMI), IDT 256 vectors, PIC 0x20-0x2F, "
		"PIT %u Hz, keyboard IRQ1\n", TICK_HZ);
	kprintf("DOORS: M1 OK\n");

	if (test_mode)
		selftest_run();
	interactive();
}
