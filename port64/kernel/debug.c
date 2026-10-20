/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Kernel debugging aids and crash reports (ROADMAP 2.4, 2.7).

   - The stack protector (-fstack-protector-strong, a global canary set at boot): a buffer that
     runs over its function's frame ends in __stack_chk_fail, a panic.
   - Stack traces with function names: the frame-pointer chain (the kernel keeps frame pointers),
     names from a table the build links in (tools/gen_ksyms.sh, a second link of the kernel).
   - kcheck(): every module that keeps lists and counters checks that they agree (sched_check,
     msg_check, pmm_check, kheap_check). The monitor's `check` and the self-tests run it.
   - Crash reports: panic writes its report (the kernel messages before it, the message and the
     trace, all from the console's ring) to a crash area on the boot disk: sectors 1984-2047, in
     the gap before the first partition, when that gap is free (the first partition starts at
     2048 or later; GRUB's core, on a system disk, ends long before). The next boot shows it once;
     the monitor's `crash` shows it again.
   - crash=NAME on the kernel command line triggers one of these on purpose (make crashtest). */

#include <doors/blk.h>
#include <doors/console.h>
#include <doors/cpu.h>
#include <doors/fs.h>
#include <doors/kernel.h>
#include <doors/kheap.h>
#include <doors/mem.h>
#include <doors/string.h>
#include <doors/x86.h>

/* ---- the stack protector ---- */

U64 __stack_chk_guard = 0x595E9FBD94FDA766ULL;	/* replaced at boot (stack_guard_init) */

NORETURN void __stack_chk_fail(void);
NORETURN void __stack_chk_fail(void)
{
	U64 off, ra = (U64)(uintptr_t)__builtin_return_address(0);
	const char *fn = ksym_lookup(ra, &off);

	panic("stack smashing detected in %s (a buffer overran into its stack canary)", fn ? fn : "?");
}

/* Called first in kmain, which never returns: no frame that saved the old canary returns. */
__attribute__((no_stack_protector)) void stack_guard_init(void)
{
	U32 lo, hi;

	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	__stack_chk_guard ^= ((U64)hi << 32 | lo) * 0x9E3779B97F4A7C15ULL;
	__stack_chk_guard &= ~0xFFULL;		/* a NUL byte first: string overruns stop there */
}

/* ---- function names and stack traces ---- */

extern const U64 ksym_addr[];		/* build/gen/ksyms.c: sorted by address */
extern const char *const ksym_name[];
extern const U32 ksym_count;
extern U8 _text_start[], _text_end[];

const char *ksym_lookup(U64 a, U64 *off)
{
	U32 lo = 0, hi = ksym_count;

	if (a < (U64)(uintptr_t)_text_start || a >= (U64)(uintptr_t)_text_end || !ksym_count)
		return NULL;
	while (hi - lo > 1) {			/* the last symbol at or below a */
		U32 mid = (lo + hi) / 2;

		if (ksym_addr[mid] <= a)
			lo = mid;
		else
			hi = mid;
	}
	if (ksym_addr[lo] > a)
		return NULL;
	*off = a - ksym_addr[lo];
	return ksym_name[lo];
}

static void frame(U64 a)
{
	U64 off = 0;
	const char *n = ksym_lookup(a, &off);

	kprintf("  %016lx %s+0x%lx\n", a, n ? n : "?", off);
}

/* rip: the first address (an exception's), or 0; rbp: the frame chain to walk. Each frame is
   { saved rbp, return address }; the walk stops at a NULL, unreadable or non-ascending frame. */
void stack_trace(U64 rip, U64 rbp)
{
	U32 i;

	kprintf("Stack trace:\n");
	if (rip)
		frame(rip);
	for (i = 0; i < 24 && rbp && !(rbp & 7); i++) {
		U64 next, ret;

		if (!probe_read((const void *)(uintptr_t)rbp, &next) ||
		    !probe_read((const void *)(uintptr_t)(rbp + 8), &ret) || !ret)
			break;
		frame(ret);
		if (next <= rbp)
			break;
		rbp = next;
	}
}

const struct regs *panic_regs;

void panic_trace(U64 rbp)
{
	if (panic_regs)
		stack_trace(panic_regs->rip, panic_regs->rbp);
	else
		stack_trace(0, rbp);
}

/* ---- invariants ---- */

void kcheck(void)
{
	const char *(*const checks[])(void) = { sched_check, msg_check, pmm_check, kheap_check };
	U64 f = irq_save();
	U32 i;

	for (i = 0; i < ARRAY_SIZE(checks); i++) {
		const char *bad = checks[i]();

		if (bad)
			panic("kernel invariant broken: %s", bad);
	}
	irq_restore(f);
}

/* ---- crash reports ---- */

#define CRASH_LBA     1984			/* .. 2047: header + 63 sectors of text */
#define CRASH_SECTORS 64
#define CRASH_TEXT    ((CRASH_SECTORS - 1) * 512)
#define CRASH_MAGIC   "DOORS CRASH"

struct crash_hdr {
	char magic[12];
	U32 version;			/* 1 */
	U32 len;			/* bytes of text after this sector */
	U32 shown;			/* a boot has shown it */
	U32 time;			/* Unix time of the panic */
	U32 csum;			/* CRC32C of the text */
	char doors[32];			/* DOORS version and build */
};

static struct blkdev *crash_dev;
static U8 cbuf[CRASH_SECTORS * 512];

/* The crash area is usable on the boot disk if its first partition starts at 2048 or later. */
static void crash_find(void)
{
	struct volume *c = vol_get('C'), *v;
	struct blkdev *d = c ? c->dev : blk_get(DEV_HD0);
	U64 first = ~0ULL;
	U32 i;

	crash_dev = NULL;
	if (!d || d->nblocks <= CRASH_LBA + CRASH_SECTORS)
		return;
	for (i = 0; (v = vol_index(i)); i++)
		if (v->dev == d && v->start < first)
			first = v->start;
	if (first != ~0ULL && first >= CRASH_LBA + CRASH_SECTORS)
		crash_dev = d;
}

void crash_save(void)
{
	struct crash_hdr *h = (struct crash_hdr *)cbuf;
	U32 n;

	if (!crash_dev || !crash_dev->panic_io) {
		kprintf("(No crash area on the boot disk: this report is on the screen and COM1 only.)\n");
		return;
	}
	memset(cbuf, 0, sizeof(cbuf));
	memcpy(h->magic, CRASH_MAGIC, sizeof(CRASH_MAGIC));
	h->version = 1;
	h->time = unix_time();
	n = (U32)strlen(doors_build);
	memcpy(h->doors, doors_build, n < sizeof(h->doors) - 1 ? n : sizeof(h->doors) - 1);
	h->len = (U32)console_ring((char *)cbuf + 512, CRASH_TEXT);
	h->csum = crc32c(~0U, cbuf + 512, h->len);
	n = 1 + (h->len + 511) / 512;
	if (crash_dev->panic_io(crash_dev, CmdWrite, CRASH_LBA, n, cbuf))
		kprintf("(Writing the crash report to disk %u failed.)\n", crash_dev->devnum);
	else
		kprintf("Crash report saved on disk %u (sectors %u-%u): the next boot shows it.\n",
			crash_dev->devnum, CRASH_LBA, CRASH_LBA + n - 1);
}

/* Read the report; true if there is a valid one. */
static bool crash_read(void)
{
	struct crash_hdr *h = (struct crash_hdr *)cbuf;
	U32 i;

	if (!crash_dev || bc_read(crash_dev, CRASH_LBA, cbuf) ||
	    memcmp(h->magic, CRASH_MAGIC, sizeof(CRASH_MAGIC)) || h->version != 1 || h->len > CRASH_TEXT)
		return false;
	for (i = 0; i < (h->len + 511) / 512; i++)
		if (bc_read(crash_dev, CRASH_LBA + 1 + i, cbuf + 512 + i * 512))
			return false;
	return crc32c(~0U, cbuf + 512, h->len) == h->csum;
}

static void crash_print(void)
{
	struct crash_hdr *h = (struct crash_hdr *)cbuf;

	h->doors[sizeof(h->doors) - 1] = 0;
	kprintf("\n*** Crash report of an earlier boot (DOORS %s, Unix time %u): ***\n", h->doors, h->time);
	console_write_n((const char *)cbuf + 512, h->len);
	kprintf("\n*** End of the crash report (the monitor's 'crash' shows it again) ***\n");
}

void crash_init(void)
{
	struct crash_hdr *h = (struct crash_hdr *)cbuf;

	crash_find();
	if (!crash_read() || h->shown)
		return;
	crash_print();
	h->shown = 1;
	bc_write(crash_dev, CRASH_LBA, cbuf);
}

void crash_show(void)
{
	if (!crash_dev) {
		kprintf("No crash area on the boot disk (its first partition starts before sector %u).\n",
			CRASH_LBA + CRASH_SECTORS);
		return;
	}
	if (!crash_read()) {
		kprintf("No crash report on disk %u.\n", crash_dev->devnum);
		return;
	}
	crash_print();
}

/* ---- crash=NAME: each aid once, on purpose ---- */

static __attribute__((noinline)) void smash(volatile U32 n)
{
	char buf[16];

	memset(buf, 'A', n);			/* n > 16: over the canary */
	kprintf("smash: %c\n", buf[0]);
}

void crash_test(const char *cmd)
{
	static const char key[] = "crash=";
	const char *p = cmd;
	char name[16];
	U32 i = 0;

	while (*p && memcmp(p, key, sizeof(key) - 1))
		p++;
	if (!*p)
		return;
	for (p += sizeof(key) - 1; *p && *p != ' ' && i < sizeof(name) - 1; p++)
		name[i++] = *p;
	name[i] = 0;
	kprintf("crash test: %s\n", name);
	if (!strcmp(name, "stack")) {
		smash(64);
	} else if (!strcmp(name, "uaf")) {		/* a freed page written, then handed out again */
		U64 pa = pmm_alloc();

		pmm_free(pa);
		*(volatile U64 *)((U8 *)phys_to_virt(pa) + 256) = 42;
		pmm_alloc();
	} else if (!strcmp(name, "double")) {
		U64 pa = pmm_alloc();

		pmm_free(pa);
		pmm_free(pa);
	} else if (!strcmp(name, "assert")) {
		volatile int one = 1;

		ASSERT(one == 2);
	} else if (!strcmp(name, "check")) {		/* a counter that no longer matches its list */
		pmm_test_corrupt();
		kcheck();
	} else if (!strcmp(name, "fault")) {		/* a kernel page fault */
		*(volatile U64 *)(uintptr_t)0x10 = 1;
	} else if (!strcmp(name, "panic")) {
		panic("crash test: a plain panic");
	}
	kprintf("crash test %s: nothing happened\n", name);
	qemu_exit(QEMU_EXIT_FAIL);
	halt_forever();
}
