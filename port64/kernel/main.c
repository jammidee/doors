/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* DOORS kernel entry.
   Called from boot/boot.asm in 64-bit mode on the kernel stack, with paging on and the
   first 1 GiB mapped both at 0 and at KERNEL_VMA. */

#include <doors/console.h>
#include <doors/cpu.h>
#include <doors/blk.h>
#include <doors/fs.h>
#include <doors/job.h>
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
	kprintf("Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines\n");
	kprintf("Based on MMURTL V1.0 by Richard A. Burgess (public domain)\n\n");
}


static void read_boot_info(U64 mbi_phys)
{
	U64 efer = rdmsr(MSR_EFER);

	mb2_parse(mbi_phys, &boot_info, true);
	test_mode = cmdline_has(boot_info.cmdline, "test");

	kprintf("Loader:  %s   Cmdline: %s\n", boot_info.loader, boot_info.cmdline);
	kprintf("Memory:  %lu MB usable (%u map entries)\n", boot_info.mem_usable >> 20, boot_info.mmap_count);
	kprintf("Kernel:  %p - %p (%lu KB)\n", _kernel_start, _kernel_end,
		(U64)(_kernel_end - _kernel_start) >> 10);
	kprintf("CPU:     long mode %s, CR0=%lx CR3=%lx CR4=%lx EFER=%lx\n",
		(efer & (1 << 10)) ? "active" : "INACTIVE", read_cr0(), read_cr3(), read_cr4(), efer);
	if (!(efer & (1 << 10)))
		panic("EFER.LMA not set");
}

/* ---- interactive mode: a tiny line monitor to try the M2 features by hand ---- */

/* "dir": read a directory with MMURTL's GetDirSector and print its entries, long names
   included (assembled from the VFAT entries before each 8.3 entry). */
static void list_dir(const char *path)
{
	static U8 sec[512];
	static const U8 pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
	char lfn[256];
	U32 s, i, k, files = 0;
	bool have_lfn = false;
	erc_t e;

	for (s = 0; (e = GetDirSector(path, (U32)strlen(path), sec, sizeof(sec), s)) == ErcOk; s++) {
		for (i = 0; i < 512; i += 32) {
			U8 *d = sec + i;
			char sn[13];
			U32 n = 0;

			if (!d[0])
				goto done;
			if (d[0] == 0xE5) {
				have_lfn = false;
				continue;
			}
			if ((d[11] & 0x3F) == ATTR_LFN) {
				U32 ord = d[0] & 0x3F;

				if (d[0] & 0x40) {
					memset(lfn, 0, sizeof(lfn));
					have_lfn = true;
				}
				for (k = 0; k < 13 && ord && (ord - 1) * 13 + k < sizeof(lfn) - 1; k++) {
					U16 u = (U16)(d[pos[k]] | (d[pos[k] + 1] << 8));

					if (u && u != 0xFFFF)
						lfn[(ord - 1) * 13 + k] = u < 128 ? (char)u : '?';
				}
				continue;
			}
			if (d[11] & ATTR_VOLUME) {
				have_lfn = false;
				continue;
			}
			for (k = 0; k < 8 && d[k] != ' '; k++)
				sn[n++] = (char)d[k];
			if (d[8] != ' ')
				sn[n++] = '.';
			for (k = 8; k < 11 && d[k] != ' '; k++)
				sn[n++] = (char)d[k];
			sn[n] = 0;
			kprintf("%-12s %10u  %04u-%02u-%02u  %s  %s\n", sn,
				(U32)(d[28] | (d[29] << 8) | (d[30] << 16) | ((U32)d[31] << 24)),
				1980 + (d[25] >> 1), ((d[25] & 1) << 3) | (d[24] >> 5), d[24] & 31,
				(d[11] & ATTR_DIR) ? "<DIR>" : "     ", have_lfn ? lfn : "");
			have_lfn = false;
			files++;
		}
	}
done:
	if (e && e != ErcNoMatch)
		kprintf("GetDirSector: error %u\n", e);
	else
		kprintf("%u entries\n", files);
}

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
			"run X  load boot module X as a new job (LoadNewJob)\n"
			"jobs   list jobs;  kill N  end job N (KillJob)\n"
			"vols   volumes;  dir C:\\PATH  list a directory;  type C:\\FILE  show a file\n"
			"run C:\\BIN\\HELLO.ELF  load a program from a file\n"
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
	} else if (!strncmp(line, "run ", 4)) {
		U32 job;
		erc_t e = LoadNewJob(line + 4, (U32)strlen(line + 4), &job);

		if (e)
			kprintf("LoadNewJob: error %u\n", e);
		else
			kprintf("Loaded %s as job %u\n", line + 4, job);
	} else if (!strcmp(line, "vols")) {
		U32 i;
		struct volume *v;

		for (i = 0; (v = vol_index(i)); i++)
			kprintf("%c:  disk %u part %u  %5lu MB  %s  %s\n", v->letter, v->dev->devnum,
				v->part, v->nsectors >> 11,
				v->kind == VOL_FAT32 ? "FAT32" : v->kind == VOL_FAT16 ? "FAT16" :
				v->kind == VOL_FAT12 ? "FAT12" : v->kind == VOL_LINUX ? "Linux" : "?",
				v->label);
	} else if (!strncmp(line, "dir ", 4)) {
		list_dir(line + 4);
	} else if (!strncmp(line, "type ", 5)) {
		static U8 buf[512];
		U32 h;
		U64 n;
		erc_t e = OpenFile(line + 5, (U32)strlen(line + 5), ModeRead, 1, &h);

		if (e) {
			kprintf("OpenFile: error %u\n", e);
		} else {
			do {
				e = ReadBytes(h, buf, sizeof(buf), &n);
				console_write_n((const char *)buf, n);
			} while (!e && n);
			CloseFile(h);
		}
	} else if (!strcmp(line, "jobs")) {
		U32 j;

		for (j = 1; j < MAX_JOBS; j++) {
			const struct job *jb = job_get(j);

			if (!jb)
				continue;
			kprintf("Job %2u  %-15s %s", j, jb->name, jb->alive ? "running" : "ended");
			if (!jb->alive)
				kprintf(" (error %u)", jb->exit_erc);
			kprintf("  tasks %u\n", job_task_count(j));
		}
		kprintf("Modules:");
		for (j = 0; j < boot_info.nmods; j++)
			kprintf(" %s", boot_info.mods[j].name);
		kprintf("\n");
	} else if (!strncmp(line, "kill ", 5)) {
		U32 j = 0;
		const char *q = line + 5;

		while (*q >= '0' && *q <= '9')
			j = j * 10 + (U32)(*q++ - '0');
		kprintf("KillJob(%u): %u\n", j, KillJob(j));
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
	read_boot_info(mbi_phys);

	pmm_init(mbi_phys, &boot_info);
	vmm_init();

	gdt_init();
	idt_init();
	pic_init();
	sched_init();		/* kmain becomes task 1; before the timer starts calling sched_tick */
	msg_init();		/* exchanges, link blocks, request blocks, services, timer blocks */
	job_init();
	syscall_init();		/* SYSCALL/SYSRET for ring 3 programs */
	pit_init(TICK_HZ);
	kbd_init();
	sti();
	kprintf("Interrupts: GDT+TSS (IST for #DF/NMI), IDT 256 vectors, PIC 0x20-0x2F, "
		"PIT %u Hz, keyboard IRQ1\n", TICK_HZ);
	kprintf("DOORS: M1 OK\n");

	/* Disks and files (timeouts need the timer, so after sti) */
	bc_init();
	ata_init();
	vol_init();
	fsys_init();

	if (test_mode)
		selftest_run();
	kprintf("Boot modules: %u\n", boot_info.nmods);
	job_load_initial();	/* programs listed in INITIAL.JOB, as MMURTL's Monitor did */
	interactive();
}
