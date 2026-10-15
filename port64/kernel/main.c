/* DOORS kernel entry.
   Called from boot/boot.asm in 64-bit mode on the kernel stack, with paging on and the
   first 1 GiB mapped both at 0 and at KERNEL_VMA. */

#include <doors/console.h>
#include <doors/cpu.h>
#include <doors/kernel.h>
#include <doors/mem.h>
#include <doors/msg.h>
#include <doors/multiboot2.h>
#include <doors/string.h>
#include <doors/task.h>
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

static struct boot_info bi;

static void boot_info(U64 mbi_phys)
{
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

/* Busy task for "spin": burns CPU (never yields) for the given number of ticks, then exits. */
static void spin_task(void *ticks)
{
	U64 end = timer_ticks() + (U64)(uintptr_t)ticks;

	while (timer_ticks() < end)
		;
}

static void cmd(const char *line)
{
	if (line[0] == '\0')
		return;
	if (!strcmp(line, "help")) {
		kprintf("help   this list\n"
			"ticks  show the 10 ms tick counter\n"
			"mem    free memory (QueryPages) and an AllocOSPage round trip\n"
			"tasks  task list and scheduler statistics\n"
			"spin   start two busy tasks for 3 s (watch time slicing in 'tasks')\n"
			"int3   breakpoint exception (handled, execution continues)\n"
			"pf     page fault at 0xdead0000 (register dump + panic)\n"
			"div0   divide error (register dump + panic)\n");
	} else if (!strcmp(line, "ticks")) {
		kprintf("Tick: %lu (%lu.%02lu s since interrupts were enabled)\n",
			timer_ticks(), timer_ticks() / TICK_HZ, timer_ticks() % TICK_HZ);
	} else if (!strcmp(line, "mem")) {
		U32 left;
		U8 *p;

		QueryPages(&left);
		kprintf("Free: %u pages (%u KB) of %lu; CR3=%lx\n", left, left * 4,
			pmm_total_pages(), read_cr3());
		if (AllocOSPage(4, &p) == ErcOk) {
			QueryPages(&left);
			kprintf("AllocOSPage(4) -> %p, free now %u; ", p, left);
			DeAllocPage(p, 4);
			QueryPages(&left);
			kprintf("after DeAllocPage %u\n", left);
		}
	} else if (!strcmp(line, "tasks")) {
		static const char *const states[] = { "free", "ready", "running", "sleeping",
						      "blocked", "dead" };
		struct sched_stats s;
		U32 i;

		kprintf("Task  Job  Pri  State     CPU ticks  Name\n");
		for (i = 0; i < MAX_TASKS; i++) {
			const struct task *t = task_by_index(i);

			if (t)
				kprintf("%4u  %3u  %3u  %-8s  %9lu  %s\n", t->tid, t->job, t->priority,
					states[t->state], t->ticks, t->name);
		}
		sched_get_stats(&s);
		kprintf("Switches %lu (preemptive %lu), idle ticks %lu, ready %u\n",
			s.switches, s.slices, s.idle_ticks, s.ready);
		{
			struct msg_stats m;

			msg_get_stats(&m);
			kprintf("Free: exchanges %u, link blocks %u, request blocks %u; "
				"timers %u, services %u\n", m.exch_free, m.lb_free, m.rqb_free,
				m.tmr_used, m.svc_used);
		}
	} else if (!strcmp(line, "spin")) {
		static erc_t e;

		task_create("spin-a", spin_task, (void *)300, 25, 1, &e);
		task_create("spin-b", spin_task, (void *)300, 25, 1, &e);
		kprintf("started spin-a and spin-b (priority 25); kmain runs at 20\n");
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
	U32 n = 0, kexch;
	U64 msg[2];

	isr_register(3, breakpoint);
	if (AllocExch(&kexch))
		panic("no exchange for the keyboard");
	kbd_set_exch(kexch);		/* keys arrive as messages; WaitMsg blocks until one does */
	kprintf("Type 'help' for commands. Keys are echoed.\n> ");
	for (;;) {
		int c;

		if (WaitMsg(kexch, msg))
			continue;
		c = (int)msg[0];
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

	pmm_init(mbi_phys, bi.mbi_size);
	vmm_init();

	gdt_init();
	idt_init();
	pic_init();
	sched_init();		/* kmain becomes task 1; before the timer starts calling sched_tick */
	msg_init();		/* exchanges, link blocks, request blocks, services, timer blocks */
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
