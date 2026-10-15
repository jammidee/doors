/* Milestone self-tests, run with kernel cmdline "test" (make test). Each prints "Mn: ... ok"
   lines to the serial log; the last step exits QEMU with QEMU_EXIT_PASS. Any failure panics,
   which exits QEMU with QEMU_EXIT_FAIL. tools/qemu_test.py drives the steps that need input. */

#include <doors/console.h>
#include <doors/cpu.h>
#include <doors/kernel.h>
#include <doors/string.h>
#include <doors/x86.h>

#define PF_TEST_ADDR 0xdead0000ULL

static volatile U32 bp_hits;
static volatile U64 bp_rip;

static void bp_handler(struct regs *r)
{
	bp_hits++;
	bp_rip = r->rip;
}

static void pf_handler(struct regs *r)
{
	U64 cr2;

	__asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
	kprintf("\n*** CPU exception 14: Page fault (expected by the self-test) ***\n");
	dump_regs(r);
	if (cr2 != PF_TEST_ADDR)
		panic("page fault at %lx, expected %llx", cr2, PF_TEST_ADDR);
	if (!(r->error & 2))
		panic("page fault error code %lx: expected a write fault", r->error);
	kprintf("M2: exception dump ok (CR2=%lx, err=%lx)\n", cr2, r->error);
	kprintf("DOORS: M2 OK\n");
	qemu_exit(QEMU_EXIT_PASS);
	halt_forever();
}

/* CMOS RTC seconds (BCD or binary, whatever the RTC uses; we only compare for change). */
static U8 rtc_seconds(void)
{
	outb(0x70, 0x00);
	return inb(0x71);
}

static void test_timer(void)
{
	U8 s0, s1;
	U64 t0, t1, n;

	/* Count PIT ticks across one RTC second: must be ~100 for a 10 ms tick. */
	s0 = rtc_seconds();
	while ((s1 = rtc_seconds()) == s0)
		hlt();
	t0 = timer_ticks();
	while (rtc_seconds() == s1)
		hlt();
	t1 = timer_ticks();
	n = t1 - t0;
	kprintf("M2: %lu ticks in one RTC second\n", n);
	if (n < 90 || n > 110)
		panic("timer rate off: %lu ticks/s, expected 100", n);
	kprintf("M2: timer ok (100 Hz)\n");
}

static void test_breakpoint(void)
{
	isr_register(3, bp_handler);
	__asm__ volatile("int3");
	__asm__ volatile("int3");
	if (bp_hits != 2)
		panic("breakpoint handler ran %u times, expected 2", bp_hits);
	kprintf("M2: breakpoint ok (2 hits, resumed after RIP=%lx)\n", bp_rip);
}

static void test_keyboard(void)
{
	char line[16];
	U32 n = 0;
	U64 deadline = timer_ticks() + 30 * 100;	/* 30 s */

	kprintf("M2: type doors\n");
	while (timer_ticks() < deadline) {
		int c = kbd_getc();

		if (c < 0) {
			hlt();
			continue;
		}
		console_putc((char)c);
		if (c == '\n')
			break;
		if (n < sizeof(line) - 1)
			line[n++] = (char)c;
	}
	line[n] = '\0';
	if (strcmp(line, "doors"))
		panic("keyboard: got '%s', expected 'doors'", line);
	kprintf("M2: keyboard ok\n");
}

void selftest_run(void)
{
	test_timer();
	test_breakpoint();
	test_keyboard();

	/* Last: a deliberate write to an unmapped address must reach the #PF handler with a
	   correct register frame. The handler reports and exits QEMU. */
	isr_register(14, pf_handler);
	*(volatile U64 *)PF_TEST_ADDR = 0x0D00125ULL;
	panic("page fault did not happen");
}
