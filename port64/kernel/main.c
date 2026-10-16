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
#include <doors/version.h>		/* generated: build/gen/doors/version.h */
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

	vid_set_title(" DOORS " DOORS_VERSION " (x86-64)  Monitor");
	serial_write(title);
	serial_write("\n");
	kprintf("Version %s, build %s\n", DOORS_VERSION, DOORS_COMMIT);
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

/* "dir": list a directory with ReadDir, so it works on FAT and ext4 alike. */
static void list_dir(const char *path)
{
	static struct dir_entry de;
	U32 i;
	erc_t e;

	for (i = 0; (e = ReadDir(path, (U32)strlen(path), i, &de)) == ErcOk; i++) {
		U64 t = de.mtime, days = t / 86400;
		/* civil_from_days (Howard Hinnant), the inverse of days_from_civil in drivers/rtc.c */
		U64 z = days + 719468, era = z / 146097, doe = z - era * 146097;
		U64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
		U64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
		U64 d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
		U64 y = yoe + era * 400 + (m <= 2);

		kprintf("%04lu-%02lu-%02lu %02lu:%02lu  %s %12lu  %s\n", y, m, d, t % 86400 / 3600,
			t % 3600 / 60, (de.attr & ATTR_DIR) ? "<DIR>" : "     ", de.size, de.name);
	}
	if (e != ErcNoMatch)
		kprintf("ReadDir: error %u\n", e);
	else
		kprintf("%u entries\n", i);
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
		kprintf("help   this list;  ver  DOORS version and build\n"
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
			"div0   divide error (register dump + panic)\n"
			"screen N  show job N's screen (SetVidOwner)\n"
			"sync   mark the ext4 volumes clean before switching off\n"
			"Keys: F1 load the CLI, F2 jobs, F3 stats, Ctrl-Alt-PgDn next screen,\n"
			"      Ctrl-Alt-Del kill the job on screen\n");
	} else if (!strncmp(line, "screen ", 7)) {
		U32 j = 0;
		const char *q = line + 7;

		while (*q >= '0' && *q <= '9')
			j = j * 10 + (U32)(*q++ - '0');
		kprintf("SetVidOwner(%u): %u\n", j, SetVidOwner(j));
	} else if (!strcmp(line, "sync")) {
		ext4_unmount();
		kprintf("ext4 volumes marked clean (safe to switch off; writing again marks them in use)\n");
	} else if (!strcmp(line, "ver")) {
		kprintf("DOORS %s (x86-64), build %s\n", DOORS_VERSION, DOORS_COMMIT);
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
				v->kind == VOL_FAT12 ? "FAT12" : v->kind == VOL_EXT4 ? "ext4 " :
				v->kind == VOL_LINUX ? "Linux" : "?",
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

/* MMURTL Monitor LoadCLI: load the CLI from the system disk and give it the screen (the keyboard
   follows). Falls back to the "cli" boot module if the disk has no \DOORS\CLI.ELF. */
static void load_cli(void)
{
	U32 job;
	erc_t erc = LoadNewJob("C:\\DOORS\\CLI.ELF", 16, &job);

	if (erc)
		erc = LoadNewJob("cli", 3, &job);
	if (erc) {
		kprintf("Can't load the CLI: error %u\n", erc);
		return;
	}
	kprintf("New CLI Job Number is: %u\n", job);
	SetVidOwner(job);
}

/* A function key runs a command (MMURTL's Monitor: F1 LdCLI, F2 Jobs, F3 Stats), then the
   prompt and the half-typed line are shown again. */
static void fkey(const char *command, const char *line, U32 n)
{
	kprintf("\n");
	if (command)
		cmd(command);
	else
		load_cli();
	kprintf("> %.*s", (int)n, line);
}

static NORETURN void interactive(void)
{
	char line[64];
	U32 n = 0, key;

	isr_register(3, breakpoint);
	/* the keyboard service already runs (kmain): keys go to the job on screen, now job 1 */
	kprintf("Type 'help' for commands. F1 CLI, F2 jobs, F3 stats, Ctrl-Alt-PgDn next screen.\n> ");
	for (;;) {
		U8 c;

		if (ReadKbd(&key, 1))
			continue;
		c = key & 0x7F;
		if (c == 0x0D) {
			console_putc('\n');
			line[n] = '\0';
			cmd(line);
			n = 0;
			kprintf("> ");
		} else if (c == 0x08) {
			if (n) {
				n--;
				console_write("\b \b");
			}
		} else if (c == 0x0F) {
			fkey(NULL, line, n);		/* F1 LdCLI */
		} else if (c == 0x10) {
			fkey("jobs", line, n);
		} else if (c == 0x11) {
			fkey("tasks", line, n);
		} else if (n < sizeof(line) - 1 && c >= ' ' && c <= '~') {
			line[n++] = (char)c;
			console_putc((char)c);
		}
	}
}

void kmain(U32 magic, U64 mbi_phys)
{
	serial_init();
	vid_init();
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
	vid_clock_base(unix_time());	/* the status line clock counts from here */

	if (test_mode)
		selftest_run();
	if (cmdline_has(boot_info.cmdline, "powercut"))		/* tools/powercut.py */
		powercut_run(boot_info.cmdline);
	if (cmdline_has(boot_info.cmdline, "pccheck"))
		powercut_check();
	kbd_service_start();		/* before INITIAL.JOB: its programs may read the keyboard */
	kprintf("Boot modules: %u\n", boot_info.nmods);
	job_load_initial();	/* programs listed in INITIAL.JOB, as MMURTL's Monitor did */
	interactive();
}
