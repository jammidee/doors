/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Milestone self-tests, run with kernel cmdline "test" (make test). Each prints "Mn: ... ok"
   lines to the serial log; the last step exits QEMU with QEMU_EXIT_PASS. Any failure panics,
   which exits QEMU with QEMU_EXIT_FAIL. tools/qemu_test.py drives the steps that need input. */

#include <doors/console.h>
#include <doors/cpu.h>
#include <doors/blk.h>
#include <doors/fs.h>
#include <doors/job.h>
#include <doors/gtable.h>
#include <doors/kernel.h>
#include <doors/kheap.h>
#include <doors/mem.h>
#include <doors/msg.h>
#include <doors/string.h>
#include <doors/task.h>
#include <doors/x86.h>

#define CHECK(cond, ...) do { if (!(cond)) panic(__VA_ARGS__); } while (0)

#define PF_TEST_ADDR 0xdead0000ULL

extern U8 _text_start[], kernel_stack_guard[], kernel_stack_top[];

/* ======================================================================== M2 */

static volatile U32 bp_hits;
static volatile U64 bp_rip;
static volatile bool pf_seen;

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
	CHECK(cr2 == PF_TEST_ADDR, "page fault at %lx, expected %llx", cr2, PF_TEST_ADDR);
	CHECK(r->error & 2, "page fault error code %lx: expected a write fault", r->error);
	CHECK(fault_fixup(r), "page fault not at a probe instruction (RIP=%lx)", r->rip);
	pf_seen = true;
}

/* CMOS RTC seconds (BCD or binary; only compared for change). */
static U8 rtc_seconds(void)
{
	outb(0x70, 0x00);
	return inb(0x71);
}

static void test_timer(void)
{
	U8 s0, s1;
	U64 t0, n;

	/* Count PIT ticks across one RTC second: must be ~100 for a 10 ms tick. */
	s0 = rtc_seconds();
	while ((s1 = rtc_seconds()) == s0)
		hlt();
	t0 = timer_ticks();
	while (rtc_seconds() == s1)
		hlt();
	n = timer_ticks() - t0;
	kprintf("M2: %lu ticks in one RTC second\n", n);
	CHECK(n >= 90 && n <= 110, "timer rate off: %lu ticks/s, expected 100", n);
	kprintf("M2: timer ok (100 Hz)\n");
}

static void test_breakpoint(void)
{
	isr_register(3, bp_handler);
	__asm__ volatile("int3");
	__asm__ volatile("int3");
	CHECK(bp_hits == 2, "breakpoint handler ran %u times, expected 2", bp_hits);
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
	CHECK(!strcmp(line, "doors"), "keyboard: got '%s', expected 'doors'", line);
	kprintf("M2: keyboard ok\n");
}

static void test_page_fault(void)
{
	/* A write to an unmapped address must reach the #PF handler with a correct frame. The
	   handler dumps it and resumes through the probe fixup. */
	isr_register(14, pf_handler);
	CHECK(!probe_write((void *)PF_TEST_ADDR, 0x0D00125ULL), "write to %llx did not fault",
	      PF_TEST_ADDR);
	isr_register(14, NULL);
	CHECK(pf_seen, "page fault handler did not run");
	kprintf("M2: exception dump ok (CR2=%llx, err=%lx)\n", PF_TEST_ADDR, last_fault_err);
	kprintf("DOORS: M2 OK\n");
}

/* ======================================================================== M3 */

static void test_pmm(void)
{
	static U64 pg[100];
	U64 free0 = pmm_free_pages(), i, j, v;

	for (i = 0; i < 100; i++) {
		pg[i] = pmm_alloc();
		CHECK(pg[i] && !(pg[i] & 0xFFF), "pmm_alloc returned %lx", pg[i]);
		CHECK(pg[i] >= DMA_LIMIT, "ordinary page %lx taken from the DMA zone", pg[i]);
		CHECK(probe_read(phys_to_virt(pg[i]), &v) && v == 0, "page %lx not zeroed", pg[i]);
		*(U64 *)phys_to_virt(pg[i]) = pg[i];
		for (j = 0; j < i; j++)
			CHECK(pg[j] != pg[i], "page %lx handed out twice", pg[i]);
	}
	CHECK(pmm_free_pages() == free0 - 100, "free count off after 100 allocations");
	for (i = 0; i < 100; i++) {
		CHECK(*(U64 *)phys_to_virt(pg[i]) == pg[i], "page %lx was overwritten", pg[i]);
		pmm_free(pg[i]);
	}
	CHECK(pmm_free_pages() == free0, "free count not restored");
	kprintf("M3: pmm ok (%lu of %lu pages free)\n", pmm_free_pages(), pmm_total_pages());
}

static void test_paging(void)
{
	static const U64 konst = 0x1234;
	U64 v, pte;

	CHECK(rdmsr(MSR_EFER) & EFER_NXE, "EFER.NXE is off");
	CHECK(read_cr3() == kernel_space.pml4, "CR3 is not the kernel PML4");
	CHECK(!probe_read((void *)0x100000, &v), "boot identity map still present");
	CHECK(probe_read(phys_to_virt(0x100000), &v), "direct map missing");
	CHECK(!probe_write(_text_start, 0), "kernel .text is writable");
	CHECK(last_fault_err == 3, ".text write: error %lx, expected 3 (present+write)", last_fault_err);
	CHECK(!probe_write((void *)&konst, 0), "kernel .rodata is writable");
	CHECK(!probe_read(kernel_stack_guard, &v), "stack guard page is mapped");
	CHECK(vmm_translate(&kernel_space, (U64)(uintptr_t)&bp_hits, NULL, &pte) && (pte & PTE_NX),
	      ".data is executable");
	CHECK(vmm_translate(&kernel_space, (U64)(uintptr_t)_text_start, NULL, &pte) &&
	      !(pte & PTE_NX) && !(pte & PTE_W), ".text permissions wrong (pte %lx)", pte);
	kprintf("M3: paging ok (no low identity map; .text RX, .rodata R, .data NX; guard page)\n");
}

static void test_os_pages(void)
{
	U32 q0, q;
	U8 *p;
	U64 v, i;

	/* The first allocation in the OS region also creates its page tables (kept for reuse),
	   so warm it up before counting pages. */
	CHECK(AllocOSPage(1, &p) == ErcOk && DeAllocPage(p, 1) == ErcOk, "AllocOSPage warm-up");
	QueryPages(&q0);
	CHECK(AllocOSPage(3, &p) == ErcOk, "AllocOSPage failed");
	CHECK((U64)(uintptr_t)p >= OS_BASE && (U64)(uintptr_t)p < OS_TOP, "AllocOSPage gave %p", p);
	for (i = 0; i < 3 * PAGE_SIZE; i += 8)
		CHECK(probe_write(p + i, i), "AllocOSPage memory not writable at +%lu", i);
	QueryPages(&q);
	CHECK(q == q0 - 3, "QueryPages: %u, expected %u", q, q0 - 3);
	CHECK(DeAllocPage(p, 3) == ErcOk, "DeAllocPage failed");
	CHECK(!probe_read(p, &v), "page still mapped after DeAllocPage");
	CHECK(DeAllocPage(p, 1) == ErcBadLinAdd, "double DeAllocPage not rejected");
	QueryPages(&q);
	CHECK(q == q0, "pages leaked: %u free, expected %u", q, q0);
	CHECK(AllocOSPage(0, &p) == ErcBadMemReq, "AllocOSPage(0) not rejected");
	kprintf("M3: AllocOSPage/DeAllocPage/QueryPages ok\n");
}

static void test_dma(void)
{
	U8 *p;
	U64 phys, ph, i;

	CHECK(AllocDMAPage(4, &p, &phys) == ErcOk, "AllocDMAPage failed");
	CHECK(phys + 4 * PAGE_SIZE <= DMA_LIMIT, "DMA memory at %lx is above 16 MB", phys);
	for (i = 0; i < 4; i++) {
		CHECK(GetPhyAdd(1, p + i * PAGE_SIZE, &ph) == ErcOk && ph == phys + i * PAGE_SIZE,
		      "DMA page %lu not contiguous (%lx)", i, ph);
		p[i * PAGE_SIZE] = (U8)i;
	}
	CHECK(DeAllocPage(p, 4) == ErcOk, "DeAllocPage of DMA pages failed");
	kprintf("M3: AllocDMAPage ok (4 contiguous pages at %lx)\n", phys);
}

static void test_spaces(void)
{
	static const char msg[] = "hello from job 2";
	struct addr_space *a, *b;
	U64 free0 = pmm_free_pages(), pa_a, pa_b, v;
	U8 *mem, *alias, *own;

	a = vmm_space_create(2);
	b = vmm_space_create(3);
	CHECK(a && b, "vmm_space_create failed");

	vmm_space_switch(a);
	CHECK(AllocPage(2, &mem) == ErcOk, "AllocPage in job 2 failed");
	CHECK((U64)(uintptr_t)mem >= USER_BASE, "AllocPage gave %p", mem);
	memcpy(mem + PAGE_SIZE - 4, msg, sizeof(msg));		/* spans both pages */
	CHECK(GetPhyAdd(2, mem, &pa_a) == ErcOk, "GetPhyAdd job 2 failed");

	vmm_space_switch(b);
	CHECK(!probe_read(mem, &v), "job 3 can see job 2's private page");
	CHECK(AliasMem(mem + PAGE_SIZE - 4, sizeof(msg), 2, &alias) == ErcOk, "AliasMem failed");
	CHECK(!memcmp(alias, msg, sizeof(msg)), "alias does not show job 2's data");
	CHECK(GetPhyAdd(3, alias - (PAGE_SIZE - 4), &pa_b) == ErcOk && pa_b == pa_a,
	      "alias maps phys %lx, expected %lx", pa_b, pa_a);
	alias[0] = 'H';						/* write through the alias */
	CHECK(AllocPage(1, &own) == ErcOk, "AllocPage in job 3 failed");
	CHECK(DeAliasMem(own, PAGE_SIZE, 2) == ErcBadAlias, "DeAliasMem accepted a non-alias");
	CHECK(DeAliasMem(alias, sizeof(msg), 2) == ErcOk, "DeAliasMem failed");
	CHECK(!probe_read(alias, &v), "alias still mapped after DeAliasMem");

	vmm_space_switch(a);
	CHECK(mem[PAGE_SIZE - 4] == 'H', "write through the alias not seen by job 2");

	vmm_space_switch(&kernel_space);
	vmm_space_destroy(a);
	vmm_space_destroy(b);
	CHECK(pmm_free_pages() == free0, "address spaces leaked %ld pages",
	      (long)(free0 - pmm_free_pages()));
	kprintf("M3: address spaces ok (private user halves, AliasMem/DeAliasMem, no leaks)\n");
}

/* ======================================================================== M4 */

static volatile U64 busy_count[2];
static volatile bool busy_stop;

static void busy(void *idx)
{
	while (!busy_stop)
		busy_count[(uintptr_t)idx]++;
}

static void test_round_robin(void)
{
	U64 free0 = pmm_free_pages(), t0, dt, lo, hi;
	U32 n0 = task_count();
	struct sched_stats s0, s1;
	erc_t e;

	busy_stop = false;
	CHECK(task_create("busy-a", busy, (void *)0, 25, 1, &e), "task_create failed (%u)", e);
	CHECK(task_create("busy-b", busy, (void *)1, 25, 1, &e), "task_create failed (%u)", e);
	CHECK(busy_count[0] == 0 && busy_count[1] == 0,
	      "priority 25 tasks ran while the priority 20 task was still running");

	sched_get_stats(&s0);
	t0 = timer_ticks();
	Sleep(50);			/* the busy tasks share the CPU meanwhile */
	dt = timer_ticks() - t0;
	sched_get_stats(&s1);

	/* kmain outranks them, so it must be back on time even though they never yield. */
	CHECK(dt >= 50 && dt <= 52, "Sleep(50) took %lu ticks", dt);
	lo = busy_count[0] < busy_count[1] ? busy_count[0] : busy_count[1];
	hi = busy_count[0] < busy_count[1] ? busy_count[1] : busy_count[0];
	CHECK(lo > 0, "a busy task never ran (%lu / %lu)", busy_count[0], busy_count[1]);
	CHECK(hi <= 3 * lo, "unfair time slicing (%lu / %lu)", busy_count[0], busy_count[1]);
	CHECK(s1.slices - s0.slices >= 5, "only %lu time-slice switches in 50 ticks",
	      s1.slices - s0.slices);

	busy_stop = true;
	Sleep(5);			/* let them see the flag and exit */
	CHECK(task_count() == n0, "tasks did not exit (%u, expected %u)", task_count(), n0);
	CHECK(pmm_free_pages() == free0, "task stacks leaked %ld pages",
	      (long)(free0 - pmm_free_pages()));
	kprintf("M4: sleep ok (Sleep(50) returned after %lu ticks while 2 tasks spun)\n", dt);
	kprintf("M4: round-robin ok (busy-a %lu, busy-b %lu loops, %lu time slices; stacks freed)\n",
		busy_count[0], busy_count[1], s1.slices - s0.slices);
}

static volatile U32 order[4], norder;

static void mark_task(void *tag)
{
	order[norder++] = (U32)(uintptr_t)tag;
}

static void test_priority(void)
{
	erc_t e;

	norder = 0;
	task_create("high", mark_task, (void *)5, 5, 1, &e);
	CHECK(norder == 1 && order[0] == 5,
	      "priority 5 task did not run before task_create returned");
	task_create("low", mark_task, (void *)30, 30, 1, &e);
	CHECK(norder == 1, "priority 30 task ran while the priority 20 task was running");
	Sleep(2);
	CHECK(norder == 2 && order[1] == 30, "priority 30 task did not run while kmain slept");
	kprintf("M4: priority ok (higher priority runs at once, lower waits until kmain blocks)\n");
}

/* Context integrity: two equal-priority tasks repeat a register-heavy calculation while being
   time-sliced against each other; every round must match kmain's reference result. */
static U64 mix(U64 n)
{
	U64 a = 1, b = 2, c = 3, d = 4, i;

	for (i = 0; i < n; i++) {
		a = a * 6364136223846793005UL + i;
		b ^= a >> 7;
		c += b * 31 + (a & 0xFF);
		d = (d << 3) ^ (c >> 5) ^ i;
	}
	return a ^ b ^ c ^ d;
}

#define MIX_N 20000

static U64 mix_ref, mix_until;
static volatile U32 mix_bad, mix_done;
static volatile U64 mix_rounds;

static void mix_task(void *unused)
{
	(void)unused;
	while (timer_ticks() < mix_until) {	/* ~20 time slices each */
		if (mix(MIX_N) != mix_ref)
			mix_bad++;
		mix_rounds++;
	}
	mix_done++;
}

static void test_context(void)
{
	struct sched_stats s0, s1;
	erc_t e;
	U64 waited = 0;

	mix_ref = mix(MIX_N);
	mix_bad = mix_done = 0;
	mix_rounds = 0;
	mix_until = timer_ticks() + 60;
	sched_get_stats(&s0);
	task_create("mix-a", mix_task, NULL, 25, 1, &e);
	task_create("mix-b", mix_task, NULL, 25, 1, &e);
	while (mix_done < 2 && waited < 3000) {
		Sleep(10);
		waited += 10;
	}
	sched_get_stats(&s1);
	CHECK(mix_done == 2, "mix tasks did not finish in 30 s");
	CHECK(mix_bad == 0, "%u rounds computed a wrong result: registers corrupted", mix_bad);
	CHECK(s1.switches - s0.switches >= 10, "too few switches (%lu) to prove anything",
	      s1.switches - s0.switches);
	CHECK(mix_rounds >= 10, "only %lu rounds computed", mix_rounds);
	kprintf("M4: context switch ok (%lu rounds by 2 tasks, all correct, across %lu switches)\n",
		mix_rounds, s1.switches - s0.switches);
}

static volatile bool newtask_ran;

static void newtask_entry(void)
{
	newtask_ran = true;
}

static void test_task_calls(void)
{
	struct sched_stats s0, s1;
	U8 p;

	CHECK(GetPriority(&p) == ErcOk && p == 20, "GetPriority: %u, expected 20", p);
	CHECK(SetPriority(nPRI) == ErcBadPriority, "SetPriority(32) not rejected");
	CHECK(SetPriority(21) == ErcOk && GetPriority(&p) == ErcOk && p == 21, "SetPriority(21)");
	SetPriority(20);
	CHECK(NewTask(1, SEL_UCODE, 10, 0, 0, 0, (U64)(uintptr_t)newtask_entry) == ErcNotSupported,
	      "NewTask with a user code segment accepted before M6");
	CHECK(NewTask(1, SEL_KCODE, 32, 0, 0, 0, (U64)(uintptr_t)newtask_entry) == ErcBadPriority,
	      "NewTask priority 32 accepted");
	CHECK(NewTask(1, SEL_KCODE, 10, 0, 0, 0, (U64)(uintptr_t)newtask_entry) == ErcOk &&
	      newtask_ran, "NewTask(priority 10) did not run at once");
	newtask_ran = false;
	CHECK(SpawnTask(newtask_entry, 22, 0, NULL, 1) == ErcOk && !newtask_ran,
	      "SpawnTask(priority 22) ran before kmain blocked");

	sched_get_stats(&s0);
	Sleep(20);			/* SpawnTask's task runs and exits, then only idle is left */
	sched_get_stats(&s1);
	CHECK(newtask_ran, "SpawnTask task never ran");
	CHECK(s1.idle_ticks - s0.idle_ticks >= 15, "idle ticks %lu during Sleep(20)",
	      s1.idle_ticks - s0.idle_ticks);
	kprintf("M4: NewTask/SpawnTask/SetPriority/GetPriority ok; idle task ran %lu of 20 ticks\n",
		s1.idle_ticks - s0.idle_ticks);
	kprintf("DOORS: M4 OK\n");
}

/* ======================================================================== M5 */

static struct msg_stats m5_base;
static U64 m5_pages;

static void test_exchanges(void)
{
	U32 e, e2;
	U64 m[2];
	struct msg_stats s;

	msg_get_stats(&m5_base);
	m5_pages = pmm_free_pages();

	CHECK(AllocExch(&e) == ErcOk, "AllocExch failed");
	CHECK(CheckMsg(e, m) == ErcNoMsg, "CheckMsg on an empty exchange");
	CHECK(SendMsg(e, 1, 2) == ErcOk && SendMsg(e, 3, 4) == ErcOk && SendMsg(e, 5, 6) == ErcOk,
	      "SendMsg failed");
	CHECK(CheckMsg(e, m) == ErcOk && m[0] == 1 && m[1] == 2, "CheckMsg: wrong first message");
	CHECK(WaitMsg(e, m) == ErcOk && m[0] == 3 && m[1] == 4, "WaitMsg: wrong second message");
	CHECK(WaitMsg(e, m) == ErcOk && m[0] == 5 && m[1] == 6, "WaitMsg: wrong third message");
	CHECK(CheckMsg(e, m) == ErcNoMsg, "exchange not empty after 3 messages");
	CHECK(SendMsg(0, 0, 0) == ErcOutOfRange && SendMsg(nEXCH, 0, 0) == ErcOutOfRange,
	      "out-of-range exchange accepted");
	CHECK(DeAllocExch(e) == ErcOk, "DeAllocExch failed");
	CHECK(SendMsg(e, 0, 0) == ErcNotAlloc && WaitMsg(e, m) == ErcNotAlloc,
	      "freed exchange still usable");

	CHECK(AllocExch(&e2) == ErcOk && SendMsg(e2, 7, 8) == ErcOk && DeAllocExch(e2) == ErcOk,
	      "DeAllocExch with a queued message");
	msg_get_stats(&s);
	CHECK(s.lb_free == m5_base.lb_free && s.exch_free == m5_base.exch_free,
	      "link blocks or exchanges leaked (%u/%u lb, %u/%u exch)", s.lb_free,
	      m5_base.lb_free, s.exch_free, m5_base.exch_free);
	kprintf("M5: exchanges ok (FIFO, CheckMsg, error codes, DeAllocExch frees queued messages)\n");
}

static U32 w_exch;
static volatile U32 w_got;

static void waiter(void *unused)
{
	U64 m[2];

	(void)unused;
	if (WaitMsg(w_exch, m) == ErcOk)
		w_got++;
}

static void test_wait_switch(void)
{
	static const U8 prio[] = { 10, 20, 30 };	/* higher, equal, lower than kmain (20) */
	erc_t e;
	U32 i;

	CHECK(AllocExch(&w_exch) == ErcOk, "AllocExch failed");
	for (i = 0; i < 3; i++) {
		w_got = 0;
		task_create("waiter", waiter, NULL, prio[i], 1, &e);
		Sleep(2);			/* let it reach WaitMsg */
		CHECK(SendMsg(w_exch, 42, 0) == ErcOk, "SendMsg to a waiting task failed");
		if (prio[i] <= 20) {
			CHECK(w_got == 1, "priority %u receiver did not run at SendMsg", prio[i]);
		} else {
			CHECK(w_got == 0, "priority %u receiver ran before kmain blocked", prio[i]);
			Sleep(2);
			CHECK(w_got == 1, "priority %u receiver never got its message", prio[i]);
		}
	}
	DeAllocExch(w_exch);
	kprintf("M5: WaitMsg/SendMsg ok (equal or higher priority receiver runs at SendMsg, "
		"lower one later)\n");
}

static U32 pp_a, pp_b;
static volatile U64 pp_bad;

static void ponger(void *n)
{
	U64 m[2], i;

	for (i = 0; i < (uintptr_t)n; i++) {
		if (WaitMsg(pp_a, m) || m[0] != i)
			pp_bad++;
		SendMsg(pp_b, m[0] + 1, 0);
	}
}

static void test_ping_pong(void)
{
	const U64 n = 10000;
	struct sched_stats s0, s1;
	U64 m[2], i, t0, bad = 0;
	erc_t e;

	CHECK(AllocExch(&pp_a) == ErcOk && AllocExch(&pp_b) == ErcOk, "AllocExch failed");
	pp_bad = 0;
	task_create("ponger", ponger, (void *)n, 20, 1, &e);
	Sleep(1);
	sched_get_stats(&s0);
	t0 = timer_ticks();
	for (i = 0; i < n; i++) {
		SendMsg(pp_a, i, 0);
		if (WaitMsg(pp_b, m) || m[0] != i + 1)
			bad++;
	}
	sched_get_stats(&s1);
	CHECK(!bad && !pp_bad, "ping-pong: %lu / %lu wrong replies", bad, pp_bad);
	Sleep(1);
	DeAllocExch(pp_a);
	DeAllocExch(pp_b);
	kprintf("M5: ping-pong ok (%lu round trips in %lu ticks, %lu task switches)\n", n,
		timer_ticks() - t0, s1.switches - s0.switches);
}

static void test_alarm(void)
{
	U32 e, own;
	U64 m[2], t0, dt;

	CHECK(GetTSSExch(&own) == ErcOk && own == current->exch && own, "GetTSSExch");
	CHECK(AllocExch(&e) == ErcOk, "AllocExch failed");
	t0 = timer_ticks();
	CHECK(Alarm(e, 5) == ErcOk, "Alarm failed");
	CHECK(WaitMsg(e, m) == ErcOk && m[0] == TIMER_MSG && m[1] == TIMER_MSG,
	      "Alarm delivered (%lx, %lx)", m[0], m[1]);
	dt = timer_ticks() - t0;
	CHECK(dt >= 5 && dt <= 6, "Alarm(5) fired after %lu ticks", dt);
	CHECK(Alarm(e, 5) == ErcOk && KillAlarm(e) == ErcOk, "KillAlarm failed");
	Sleep(10);
	CHECK(CheckMsg(e, m) == ErcNoMsg, "a killed alarm still fired");
	CHECK(Alarm(0, 1) == ErcOutOfRange, "Alarm on exchange 0 accepted");
	DeAllocExch(e);
	kprintf("M5: Alarm/KillAlarm ok (Alarm(5) fired after %lu ticks; Sleep uses the same timer "
		"blocks)\n", dt);
}

/* Keys arrive as messages sent with ISendMsg from the keyboard IRQ; an Alarm on the same
   exchange is the timeout. */
static void test_kbd_messages(void)
{
	char line[16];
	U32 e, n = 0;
	U64 m[2];

	CHECK(AllocExch(&e) == ErcOk, "AllocExch failed");
	kbd_set_exch(e);
	Alarm(e, 30 * 100);
	kprintf("M5: type msg\n");
	for (;;) {
		CHECK(WaitMsg(e, m) == ErcOk, "WaitMsg on the keyboard exchange failed");
		CHECK(m[0] != TIMER_MSG, "timed out waiting for keyboard messages");
		if (m[0] == '\n')
			break;
		if (n < sizeof(line) - 1)
			line[n++] = (char)m[0];
	}
	line[n] = '\0';
	kbd_set_exch(0);
	KillAlarm(e);
	DeAllocExch(e);
	CHECK(!strcmp(line, "msg"), "keyboard messages: got '%s', expected 'msg'", line);
	kprintf("M5: keyboard ISendMsg ok (keys delivered as messages from the IRQ)\n");
}

/* ---- Request / Respond with a NUMBERS service in the same job (MSAMPLES/SERVICE) ---- */

static U32 num_exch;
static volatile U32 num_d[3];

static void numbers_service(void *unused)
{
	U64 m[2], next = 0;

	(void)unused;
	AllocExch(&num_exch);
	RegisterSvc("NUMBERS ", num_exch);
	for (;;) {
		struct RqBlk *r;

		if (WaitMsg(num_exch, m))
			continue;
		r = (struct RqBlk *)(uintptr_t)m[0];
		if (r->ServiceCode == 1) {
			*(U64 *)r->pData1 = next++;
			num_d[0] = r->dData0;
			num_d[1] = r->dData1;
			num_d[2] = r->dData2;
			Respond(m[0], ErcOk);
		} else if (r->ServiceCode == 0) {	/* shut down */
			UnRegisterSvc("NUMBERS ");
			Respond(m[0], ErcOk);
			DeAllocExch(num_exch);
			return;
		} else {
			Respond(m[0], ErcBadSvcCode);
		}
	}
}

static void test_request(void)
{
	U32 resp;
	U64 h, m[2], num = 99;
	erc_t e;
	struct msg_stats s;

	CHECK(task_create("numbers", numbers_service, NULL, 15, 1, &e), "service task");
	CHECK(AllocExch(&resp) == ErcOk, "AllocExch failed");

	CHECK(Request("NUMBERS ", 1, resp, &h, 0, (U8 *)&num, sizeof(num), NULL, 0, 7, 8, 9) == ErcOk,
	      "Request failed");
	CHECK(WaitMsg(resp, m) == ErcOk && m[0] == h && m[1] == ErcOk, "response (%lx, %lu)",
	      m[0], m[1]);
	CHECK(num == 0, "first number %lu", num);
	CHECK(num_d[0] == 7 && num_d[1] == 8 && num_d[2] == 9,
	      "dData0-2 arrived as %u,%u,%u (MMURTL bug: dData2 = dData0)", num_d[0], num_d[1],
	      num_d[2]);
	CHECK(Request("NUMBERS ", 1, resp, &h, 0, (U8 *)&num, sizeof(num), NULL, 0, 0, 0, 0) == ErcOk &&
	      WaitMsg(resp, m) == ErcOk && num == 1, "second number %lu", num);
	CHECK(Request("NUMBERS ", 5, resp, &h, 0, NULL, 0, NULL, 0, 0, 0, 0) == ErcOk &&
	      WaitMsg(resp, m) == ErcOk && m[1] == ErcBadSvcCode, "bad service code: %lu", m[1]);
	CHECK(Request("NOSUCH  ", 1, resp, &h, 0, NULL, 0, NULL, 0, 0, 0, 0) == ErcNoSuchSvc,
	      "unknown service accepted");
	CHECK(Respond(12345, 0) == ErcNotRqBlk, "Respond with a bogus handle accepted");
	CHECK(Request("NUMBERS ", 0, resp, &h, 0, NULL, 0, NULL, 0, 0, 0, 0) == ErcOk &&
	      WaitMsg(resp, m) == ErcOk, "shutdown request");
	CHECK(Request("NUMBERS ", 1, resp, &h, 0, NULL, 0, NULL, 0, 0, 0, 0) == ErcNoSuchSvc,
	      "service still registered after UnRegisterSvc");
	DeAllocExch(resp);
	Sleep(2);
	msg_get_stats(&s);
	CHECK(s.rqb_free == m5_base.rqb_free && s.svc_used == m5_base.svc_used,
	      "request blocks or services leaked");
	kprintf("M5: Request/Respond ok (NUMBERS service; dData0-2 = 7,8,9 intact; error codes)\n");
}

/* ---- the same across address spaces: client in job 2, service + worker in job 3 ---- */

static U32 up_svc, up_work;
static volatile bool up_ready, up_done, up_alias_ok, up_alias_gone, up_move_checked;

static void upcase_worker(void *unused)
{
	U64 m[2], v;

	(void)unused;
	for (;;) {
		struct RqBlk *r;
		U8 *alias;
		U64 i, pa_alias, pa_orig;

		if (WaitMsg(up_work, m))
			return;
		if (m[0] == 0xDEAD)			/* plain message from the service: quit */
			return;
		r = (struct RqBlk *)(uintptr_t)m[0];
		alias = r->pData1;
		up_alias_ok = r->svc_job == 3 &&
			GetPhyAdd(3, r->pData1, &pa_alias) == ErcOk &&
			GetPhyAdd(2, r->pOrig1, &pa_orig) == ErcOk && pa_alias == pa_orig;
		for (i = 0; i < r->cbData1 && i < r->cbData2; i++) {
			U8 c = r->pData1[i];
			r->pData2[i] = (c >= 'a' && c <= 'z') ? (U8)(c - 32) : c;
		}
		Respond(m[0], ErcOk);
		up_alias_gone = !probe_read(alias, &v);	/* Respond removed the alias */
	}
}

static void upcase_service(void *unused)
{
	U64 m[2];
	U32 kexch = 1;		/* an exchange this job does not own */
	erc_t e;

	(void)unused;
	AllocExch(&up_svc);
	AllocExch(&up_work);
	task_create("up-work", upcase_worker, NULL, 15, 3, &e);
	RegisterSvc("UPCASE  ", up_svc);
	up_ready = true;
	for (;;) {
		struct RqBlk *r;

		if (WaitMsg(up_svc, m))
			continue;
		r = (struct RqBlk *)(uintptr_t)m[0];
		if (r->ServiceCode == 0) {
			UnRegisterSvc("UPCASE  ");
			SendMsg(up_work, 0xDEAD, 0);
			Respond(m[0], ErcOk);
			Sleep(2);
			DeAllocExch(up_work);
			DeAllocExch(up_svc);
			return;
		}
		up_move_checked = MoveRequest(m[0], kexch) == ErcNotOwner;
		MoveRequest(m[0], up_work);		/* the worker answers it */
	}
}

static void upcase_client(void *unused)
{
	static const char text[] = "doors messaging";
	U8 *buf;
	U32 resp;
	U64 h, m[2];

	(void)unused;
	if (AllocPage(1, &buf) || AllocExch(&resp))
		return;
	memcpy(buf, text, sizeof(text));
	if (Request("UPCASE  ", 1, resp, &h, 1, buf, sizeof(text), buf + 512, sizeof(text),
		    0, 0, 0) == ErcOk && WaitMsg(resp, m) == ErcOk && m[1] == ErcOk &&
	    !memcmp(buf + 512, "DOORS MESSAGING", sizeof(text)))
		up_done = true;
	Request("UPCASE  ", 0, resp, &h, 0, NULL, 0, NULL, 0, 0, 0, 0);
	WaitMsg(resp, m);
	DeAllocExch(resp);
}

static void test_cross_job(void)
{
	struct addr_space *a = vmm_space_create(2), *b = vmm_space_create(3);
	struct msg_stats s;
	erc_t e;
	U32 waited, n0 = task_count();	/* kmain, idle and system services */

	CHECK(a && b, "vmm_space_create failed");
	task_create("up-svc", upcase_service, NULL, 15, 3, &e);
	CHECK(up_ready, "UPCASE service did not start");
	task_create("up-client", upcase_client, NULL, 18, 2, &e);
	for (waited = 0; task_count() > n0 && waited < 300; waited++)
		Sleep(1);
	CHECK(up_done, "cross-job request failed or returned wrong data");
	CHECK(up_alias_ok, "service did not see the client's buffer through an alias");
	CHECK(up_move_checked, "MoveRequest to another job's exchange was not refused");
	CHECK(up_alias_gone, "alias still mapped after Respond");
	CHECK(task_count() == n0, "tasks left over: %u, expected %u", task_count(), n0);

	vmm_space_destroy(a);
	vmm_space_destroy(b);
	msg_get_stats(&s);
	CHECK(s.exch_free == m5_base.exch_free && s.lb_free == m5_base.lb_free &&
	      s.rqb_free == m5_base.rqb_free && s.svc_used == m5_base.svc_used && !s.tmr_used,
	      "messaging objects leaked (exch %u/%u, lb %u/%u, rqb %u/%u, svc %u, tmr %u)",
	      s.exch_free, m5_base.exch_free, s.lb_free, m5_base.lb_free, s.rqb_free,
	      m5_base.rqb_free, s.svc_used, s.tmr_used);
	CHECK(pmm_free_pages() == m5_pages, "pages leaked: %ld", (long)(m5_pages - pmm_free_pages()));
	kprintf("M5: cross-job Request ok (job 2 client -> job 3 service, buffers aliased, "
		"MoveRequest to a worker, aliases removed by Respond)\n");
	kprintf("M5: no leaks (exchanges, link blocks, request blocks, services, timers, pages)\n");
	kprintf("DOORS: M5 OK\n");
}

/* ======================================================================== M6 */

/* KECHO: a kernel service user programs can call. pData1/pData2 come from the user job and are
   aliased into job 1 by the kernel's WaitMsg. */
static U32 kecho_exch;

static void kecho_service(void *unused)
{
	static const char prefix[] = "kernel got: ";
	U64 m[2];

	(void)unused;
	AllocExch(&kecho_exch);
	RegisterSvc("KECHO   ", kecho_exch);
	for (;;) {
		struct RqBlk *r;
		U64 i, n = 0;

		if (WaitMsg(kecho_exch, m) && !m[0])
			continue;
		r = (struct RqBlk *)(uintptr_t)m[0];
		for (i = 0; i < sizeof(prefix) - 1 && n < r->cbData2; i++)
			r->pData2[n++] = (U8)prefix[i];
		for (i = 0; i < r->cbData1 && n < r->cbData2; i++)
			r->pData2[n++] = r->pData1[i];
		if (n < r->cbData2)
			r->pData2[n] = 0;
		Respond(m[0], ErcOk);
	}
}

static const struct job *wait_job(U32 job, U32 ticks)
{
	const struct job *j = job_get(job);

	while (j && j->alive && ticks--)
		Sleep(1);
	return j;
}

static U32 run(const char *name)
{
	U32 job = 0;
	erc_t e = LoadNewJob(name, (U32)strlen(name), &job);

	CHECK(e == ErcOk, "LoadNewJob(%s): error %u", name, e);
	return job;
}

static struct msg_stats m6_base;
static U64 m6_pages;

static void test_user_hello(void)
{
	const struct job *j;
	erc_t e;
	U32 job;

	task_create("kecho", kecho_service, NULL, 15, 1, &e);
	CHECK(svc_registered("KECHO   "), "KECHO service did not start");

	CHECK(LoadNewJob("nosuch", 6, &job) == ErcNoSuchFile, "LoadNewJob of a missing program");
	CHECK(LoadNewJob("INITIAL.JOB", 11, &job) == ErcBadRunFile, "a text file loaded as a program");
	job = run("hello");
	j = wait_job(job, 500);
	CHECK(j && !j->alive, "hello did not finish");
	CHECK(j->exit_erc == ErcOk, "hello exited with %u", j->exit_erc);

	/* Baseline for the leak check, taken after hello: its KECHO request made KECHO (job 1)
	   alias a user buffer, which created page tables in job 1's user half. Those are kept for
	   reuse (like the OS region's tables), so they are not counted as a leak. */
	msg_get_stats(&m6_base);
	m6_pages = pmm_free_pages();
	kprintf("M6: hello ok (job %u ran in ring 3, used SYSCALL, a kernel service, and exited 0)\n",
		job);
}

static void test_user_badptr(void)
{
	const struct job *j;
	U32 job = run("badptr");

	j = wait_job(job, 500);
	CHECK(j && !j->alive, "badptr did not end");
	CHECK(j->exit_erc == ErcCpuFault + 14, "badptr ended with %u, expected %u (page fault)",
	      j->exit_erc, ErcCpuFault + 14);
	kprintf("M6: bad pointers ok (rejected with errors; the faulting job was ended, the kernel "
		"kept running)\n");
}

static void test_user_fpu(void)
{
	const struct job *a, *b;
	U32 ja = run("fpu"), jb = run("fpu");

	a = wait_job(ja, 1000);
	b = wait_job(jb, 1000);
	CHECK(a && b && !a->alive && !b->alive, "fpu jobs did not finish");
	CHECK(a->exit_erc == 0 && b->exit_erc == 0, "fpu jobs exited with %u / %u", a->exit_erc,
	      b->exit_erc);
	kprintf("M6: FPU/SSE ok (two jobs, results bit-identical across task switches)\n");
}

static void test_user_service(void)
{
	const struct job *s, *c;
	U32 js = run("upsvc"), jc, t;

	for (t = 0; t < 200 && !svc_registered("UPCASE  "); t++)
		Sleep(1);
	CHECK(svc_registered("UPCASE  "), "user-mode UPCASE service did not register");
	jc = run("upcli");
	c = wait_job(jc, 500);
	s = wait_job(js, 500);
	CHECK(c && !c->alive && c->exit_erc == 0, "upcli failed (%u)", c ? c->exit_erc : 0);
	CHECK(s && !s->alive && s->exit_erc == 0, "upsvc failed (%u)", s ? s->exit_erc : 0);
	kprintf("M6: user-mode service ok (job %u served job %u through Request/Respond)\n", js, jc);
}

static void test_killjob(void)
{
	const struct job *j;
	U32 job = run("spinner");
	struct msg_stats s;

	Sleep(20);
	CHECK(job_task_count(job) == 2, "spinner has %u tasks, expected 2", job_task_count(job));
	CHECK(KillJob(job) == ErcOk, "KillJob failed");
	j = job_get(job);
	CHECK(!j->alive && j->exit_erc == ErcOpCancel, "killed job: alive %u, error %u", j->alive,
	      j->exit_erc);
	Sleep(2);				/* let the zombies be reaped */
	CHECK(job_task_count(job) == 0, "%u tasks of the killed job left", job_task_count(job));
	CHECK(KillJob(job) == ErcBadJobNum, "KillJob of a dead job accepted");

	msg_get_stats(&s);
	CHECK(s.exch_free == m6_base.exch_free && s.lb_free == m6_base.lb_free &&
	      s.rqb_free == m6_base.rqb_free && s.svc_used == m6_base.svc_used && !s.tmr_used,
	      "messaging objects leaked (exch %u/%u, lb %u/%u, rqb %u/%u, svc %u/%u, tmr %u)",
	      s.exch_free, m6_base.exch_free, s.lb_free, m6_base.lb_free, s.rqb_free,
	      m6_base.rqb_free, s.svc_used, m6_base.svc_used, s.tmr_used);
	CHECK(pmm_free_pages() == m6_pages, "pages leaked by 6 jobs: %ld",
	      (long)(m6_pages - pmm_free_pages()));
	kprintf("M6: KillJob ok (2 tasks, exchange and memory of the spinning job reclaimed)\n");
	kprintf("M6: no leaks after 6 more jobs (pages, exchanges, link blocks, request blocks)\n");
	kprintf("DOORS: M6 OK\n");
}

/* ======================================================================== 3.1 */

static void test_kheap(void)
{
	static void *blk[600];
	U64 b0, p0, b, p;
	U32 i, k;

	kheap_stats(&b0, &p0);
	for (k = 0; k < 2; k++) {
		for (i = 0; i < ARRAY_SIZE(blk); i++) {
			U32 n = 1 + (i * 37) % (i & 1 ? 6000 : 2100), j;
			U8 *q = kmalloc(n);

			CHECK(q, "kmalloc(%u) failed", n);
			for (j = 0; j < n; j++)
				CHECK(!q[j], "kmalloc(%u): byte %u not zero", n, j);
			memset(q, (int)i, n);
			blk[i] = q;
		}
		for (i = 0; i < ARRAY_SIZE(blk); i += 2)	/* half, then the rest */
			kfree(blk[i]);
		for (i = 1; i < ARRAY_SIZE(blk); i += 2) {
			CHECK(((U8 *)blk[i])[0] == (U8)i, "kmalloc block %u overwritten", i);
			kfree(blk[i]);
		}
		kcheck();
	}
	kheap_stats(&b, &p);
	CHECK(b == b0, "kheap: %lu blocks still in use", b - b0);
	kprintf("3.1: kernel heap ok (%u blocks of 1..6000 bytes, twice; %lu pages kept for reuse)\n",
		(U32)ARRAY_SIZE(blk), p);
}

#define MANY 1000

/* One round: start MANY idle jobs at once (the job, task and exchange tables grow), end them all. */
static void many_jobs(U32 *job)
{
	U32 i, left, t;

	for (i = 0; i < MANY; i++)
		job[i] = run("idle");
	Sleep(10);
	for (i = 0, left = 0; i < MANY; i++)
		left += job_get(job[i]) && job_get(job[i])->alive;
	CHECK(left == MANY, "only %u of %u idle jobs are running", left, MANY);
	kcheck();
	for (i = 0; i < MANY; i++)
		CHECK(KillJob(job[i]) == ErcOk, "KillJob(%u) failed", job[i]);
	for (t = 0; t < 500; t++) {			/* let the zombies be reaped */
		for (i = 0, left = 0; i < MANY; i++)
			left += job_task_count(job[i]);
		if (!left)
			break;
		Sleep(1);
	}
	CHECK(!left, "%u tasks of %u killed jobs left", left, MANY);
}

static void test_many_jobs(void)
{
	static U32 job[MANY];
	struct msg_stats s0, s;
	U64 p0, p, t0 = timer_ticks();

	/* Round 1 grows the tables, round 2 may still make a page table the kernel keeps (the
	   kernel stacks land elsewhere); round 3 must give back exactly what it took. */
	many_jobs(job);
	many_jobs(job);
	msg_get_stats(&s0);
	p0 = pmm_free_pages() + gt_pages_total();
	many_jobs(job);
	msg_get_stats(&s);
	p = pmm_free_pages() + gt_pages_total();
	CHECK(s.exch_free == s0.exch_free && s.lb_free == s0.lb_free && s.rqb_free == s0.rqb_free &&
	      !s.tmr_used, "messaging objects leaked by %u jobs (exch %u/%u, lb %u/%u, rqb %u/%u)", MANY,
	      s.exch_free, s0.exch_free, s.lb_free, s0.lb_free, s.rqb_free, s0.rqb_free);
	CHECK(p == p0, "pages leaked by %u jobs: %ld", MANY, (long)(p0 - p));
	kcheck();
	kprintf("3.1: %u jobs at once ok (3 rounds, %lu.%lu s; job numbers reused, tables at %u jobs, "
		"%lu pages; no leaks)\n", MANY, (timer_ticks() - t0) / 100, (timer_ticks() - t0) / 10 % 10,
		job_limit(), gt_pages_total());
}

/* ======================================================================== M7 */

/* The test disk is made by tools/mkdisk.py and checked afterwards by tools/check_disk.py. */

#define S(x) x, (U32)(sizeof(x) - 1)		/* "literal", its length */

static U8 fbuf[4096], fbuf2[4096];

static U8 patb(U64 i, U32 mul) { return (U8)(i * mul); }

static erc_t write_pat(const char *path, U32 plen, U64 n, U32 mul)
{
	U32 h;
	U64 done, off, i, k;
	erc_t erc;

	erc = CreateFile(path, plen, 0);
	if (erc && erc != ErcDupName)
		return erc;
	if ((erc = OpenFile(path, plen, ModeModify, 1, &h)))
		return erc;
	for (off = 0; off < n && !erc; off += k) {
		k = n - off < sizeof(fbuf) ? n - off : sizeof(fbuf);
		for (i = 0; i < k; i++)
			fbuf[i] = patb(off + i, mul);
		erc = WriteBytes(h, fbuf, k, &done);
	}
	CloseFile(h);
	return erc;
}

static bool check_pat(const char *path, U32 plen, U64 n, U32 mul)
{
	U32 h;
	U64 done, off = 0, i, size;
	erc_t erc;

	if (OpenFile(path, plen, ModeRead, 1, &h))
		return false;
	if (GetFileSize(h, &size) || size != n) {
		CloseFile(h);
		return false;
	}
	do {
		erc = ReadBytes(h, fbuf, sizeof(fbuf), &done);
		for (i = 0; i < done; i++)
			if (fbuf[i] != patb(off + i, mul)) {
				CloseFile(h);
				return false;
			}
		off += done;
	} while (erc == ErcOk && done);
	CloseFile(h);
	return off == n;
}

static U64 read_all(const char *path, U32 plen, U8 *buf, U64 max)
{
	U32 h;
	U64 n = 0;
	erc_t erc;

	if (OpenFile(path, plen, ModeRead, 1, &h))
		return ~0UL;
	erc = ReadBytes(h, buf, max, &n);
	CloseFile(h);
	return erc == ErcOk || erc == ErcEOF ? n : ~0UL;
}

static void test_disk(void)
{
	struct blkdev *d = blk_get(DEV_HD0);
	struct devstat ds;
	struct volume *c = vol_get('C'), *dv = vol_get('D'), *e = vol_get('E');
	U32 n, i;
	U64 scratch;

	CHECK(d, "no disk 12 (is the test disk attached?)");
	CHECK(d->nblocks == 262144, "disk 12 has %lu blocks, expected 262144", d->nblocks);
	CHECK(DeviceStat(DEV_HD0, (U8 *)&ds, sizeof(ds), &n) == ErcOk && ds.nblocks == d->nblocks,
	      "DeviceStat");
	CHECK(DeviceOp(DEV_HD0, CmdRead, 0, 1, fbuf) == ErcOk && fbuf[510] == 0x55 && fbuf[511] == 0xAA,
	      "DeviceOp read of the MBR");
	scratch = d->nblocks - 8;			/* unpartitioned space at the end */
	for (i = 0; i < 1024; i++)
		fbuf[i] = (U8)(i ^ 0x5A);
	n = (U32)DeviceOp(DEV_HD0, CmdWrite, scratch, 2, fbuf);
	CHECK(n == ErcOk, "DeviceOp write: erc %u", n);
	memset(fbuf2, 0, 1024);
	CHECK(DeviceOp(DEV_HD0, CmdRead, scratch, 2, fbuf2) == ErcOk && !memcmp(fbuf, fbuf2, 1024),
	      "DeviceOp read back");
	CHECK(DeviceOp(DEV_HD0, CmdVerify, scratch, 2, NULL) == ErcOk, "DeviceOp verify");
	CHECK(DeviceOp(99, CmdRead, 0, 1, fbuf) == ErcBadDevNum, "bad device number");
	CHECK(DeviceOp(DEV_HD0 + 3, CmdRead, 0, 1, fbuf) == ErcNoDriver, "missing device");
	CHECK(DeviceOp(DEV_HD0, 7, 0, 1, fbuf) == ErcBadOpNum, "bad operation");
	CHECK(DeviceOp(DEV_HD0, CmdRead, d->nblocks, 1, fbuf) == ErcBeyondEOF, "read past the end");
	CHECK(c && c->kind == VOL_FAT32 && dv && dv->kind == VOL_FAT16 && e && e->kind == VOL_FAT12,
	      "volumes C:/D:/E: are not FAT32/16/12");
	kprintf("M7: disk ok (ATA disk 12, %lu MB; DeviceOp/DeviceStat; C: FAT32 %s, D: FAT16 %s, "
		"E: FAT12 %s)\n", d->nblocks >> 11, c->label, dv->label, e->label);
}

static void test_fat_read(void)
{
	U32 h;
	U64 n;
	U8 disk;

	CHECK(read_all(S("C:\\HELLO.TXT"), fbuf, sizeof(fbuf)) == 25 &&
	      !memcmp(fbuf, "Hello from a FAT32 file.\n", 25), "C:\\HELLO.TXT");
	CHECK(read_all(S("c:\\a long file name.TXT"), fbuf, sizeof(fbuf)) == 27 &&
	      !memcmp(fbuf, "This file has a long name.\n", 27), "long name, any case");
	CHECK(read_all(S("C:\\ALONGF~1.TXT"), fbuf, sizeof(fbuf)) == 27, "short alias of a long name");
	CHECK(read_all(S("C:/dir1/sub/deep.txt"), fbuf, sizeof(fbuf)) == 19 &&
	      !memcmp(fbuf, "Three levels down.\n", 19), "nested directories");
	CHECK(read_all(S("C:\\DIR1\\SUB\\..\\..\\HELLO.TXT"), fbuf, sizeof(fbuf)) == 25, "'..' in a path");
	CHECK(read_all(S("D:\\README.TXT"), fbuf, sizeof(fbuf)) == 16 && !memcmp(fbuf, "FAT16 volume D:\n", 16),
	      "D:\\README.TXT (FAT16)");
	CHECK(read_all(S("E:\\TINY.TXT"), fbuf, sizeof(fbuf)) == 16 && !memcmp(fbuf, "FAT12 volume E:\n", 16),
	      "E:\\TINY.TXT (FAT12)");
	{
		U64 off = 0, i;
		bool ok = OpenFile(S("C:\\BIG.BIN"), ModeRead, 1, &h) == ErcOk;

		while (ok) {
			erc_t erc = ReadBytes(h, fbuf, sizeof(fbuf), &n);

			for (i = 0; i < n; i++)
				ok &= fbuf[i] == (U8)((off + i) * 7 + (off + i) / 251);
			off += n;
			if (erc)
				break;
		}
		CloseFile(h);
		CHECK(ok && off == 200000, "C:\\BIG.BIN (200000 bytes, many clusters): %lu bytes", off);
	}
	CHECK(OpenFile(S("C:\\NOPE.TXT"), ModeRead, 1, &h) == ErcNoSuchFile, "missing file");
	CHECK(OpenFile(S("C:\\NODIR\\X.TXT"), ModeRead, 1, &h) == ErcNoSuchDir, "missing directory");
	CHECK(OpenFile(S("Z:\\X.TXT"), ModeRead, 1, &h) == ErcNoSuchDrive, "missing drive");
	CHECK(OpenFile(S("HELLO.TXT"), ModeRead, 1, &h) == ErcBadFileSpec, "path without a drive");
	CHECK(OpenFile(S("C:\\DIR1"), ModeRead, 1, &h) == ErcNotAFile, "opening a directory");
	CHECK(OpenFile(S("C:\\HELLO.TXT"), 5, 1, &h) == ErcBadOpenMode, "bad mode");

	CHECK(OpenFile(S("C:\\BIG.BIN"), ModeRead, 0, &h) == ErcOk, "block mode open");
	CHECK(ReadBlock(h, fbuf, 512, 1024, &n) == ErcOk && n == 512 &&
	      fbuf[0] == (U8)(1024 * 7 + 1024 / 251), "ReadBlock at 1024");
	CHECK(ReadBlock(h, fbuf, 512, 200192, &n) == ErcBeyondEOF, "ReadBlock beyond EOF");
	CHECK(ReadBlock(h, fbuf, 100, 0, &n) == ErcBadFileSpec, "ReadBlock of a partial block");
	CHECK(ReadBytes(h, fbuf, 10, &n) == ErcBlockFile, "ReadBytes on a block file");
	CloseFile(h);

	CHECK(GetDirSector(S("C:\\"), fbuf, 512, 0) == ErcOk, "GetDirSector C:\\");
	CHECK(GetDirSector(S("C:\\DIR1"), fbuf, 512, 0) == ErcOk && !memcmp(fbuf, ".          ", 11) &&
	      !memcmp(fbuf + 32, "..         ", 11), "GetDirSector C:\\DIR1 starts with . and ..");
	CHECK(GetDirSector(S("C:\\DIR1"), fbuf, 512, 999) == ErcNoMatch, "GetDirSector past the end");
	CHECK(GetSystemDisk(&disk) == ErcOk && disk == 2, "GetSystemDisk");
	kprintf("M7: FAT read ok (FAT32/16/12, long names, nested dirs, 200000-byte file, block mode)\n");
}

static void test_fat_write(void)
{
	U32 h, h2;
	U64 n, size, lfa;

	CHECK(CreateFile(S("C:\\NEW.TXT"), 0) == ErcOk, "CreateFile");
	CHECK(CreateFile(S("C:\\NEW.TXT"), 0) == ErcDupName, "CreateFile of an existing name");
	CHECK(OpenFile(S("C:\\NEW.TXT"), ModeModify, 1, &h) == ErcOk, "OpenFile modify");
	CHECK(OpenFile(S("C:\\NEW.TXT"), ModeRead, 1, &h2) == ErcFileInUse, "read while modifying");
	{
		U64 i;

		for (i = 0; i < 4000; i++)
			fbuf[i] = patb(i, 13);
		CHECK(WriteBytes(h, fbuf, 4000, &n) == ErcOk && n == 4000, "WriteBytes 4000");
		for (i = 0; i < 4000; i++)
			fbuf[i] = patb(4000 + i, 13);
		CHECK(WriteBytes(h, fbuf, 4000, &n) == ErcOk, "WriteBytes 4000 more");
		for (i = 0; i < 2000; i++)
			fbuf[i] = patb(8000 + i, 13);
		CHECK(WriteBytes(h, fbuf, 2000, &n) == ErcOk, "WriteBytes 2000 more");
	}
	CHECK(GetFileLFA(h, &lfa) == ErcOk && lfa == 10000, "GetFileLFA after writing: %lu", lfa);
	CHECK(SetFileLFA(h, 20000) == ErcBeyondEOF, "SetFileLFA beyond EOF");
	CHECK(SetFileLFA(h, ~0UL) == ErcOk && GetFileLFA(h, &lfa) == ErcOk && lfa == 10000, "SetFileLFA(EOF)");
	CHECK(GetFileSize(h, &size) == ErcOk && size == 10000, "GetFileSize");
	CloseFile(h);
	CHECK(check_pat(S("C:\\NEW.TXT"), 10000, 13), "read back NEW.TXT");

	/* copies: stream mode, and block mode + SetFileSize (MMURTL's copy lost the last block) */
	n = read_all(S("C:\\HELLO.TXT"), fbuf2, sizeof(fbuf2));
	CHECK(CreateFile(S("C:\\COPY.TXT"), 0) == ErcOk && OpenFile(S("C:\\COPY.TXT"), ModeModify, 1, &h) == ErcOk &&
	      WriteBytes(h, fbuf2, n, &size) == ErcOk && size == 25, "stream copy");
	CloseFile(h);
	memset(fbuf2 + 25, 0, 512 - 25);
	CHECK(CreateFile(S("C:\\COPYB.TXT"), 0) == ErcOk && OpenFile(S("C:\\COPYB.TXT"), ModeModify, 0, &h) == ErcOk &&
	      WriteBlock(h, fbuf2, 512, 0, &size) == ErcOk && SetFileSize(h, 25) == ErcOk, "block copy");
	CHECK(GetFileSize(h, &size) == ErcOk && size == 25, "block copy size %lu", size);
	CloseFile(h);

	CHECK(write_pat(S("C:\\Long name created by DOORS.txt"), 0, 1) == ErcOk, "long name create");
	CHECK(OpenFile(S("C:\\long NAME created by doors.TXT"), ModeModify, 1, &h) == ErcOk &&
	      WriteBytes(h, (const U8 *)"long\n", 5, &n) == ErcOk, "long name write");
	CloseFile(h);

	CHECK(write_pat(S("C:\\TRUNC.TXT"), 3000, 3) == ErcOk, "TRUNC.TXT");
	CHECK(OpenFile(S("C:\\TRUNC.TXT"), ModeModify, 1, &h) == ErcOk && SetFileSize(h, 100) == ErcOk &&
	      SetFileSize(h, 5000) == ErcOk && GetFileSize(h, &size) == ErcOk && size == 5000,
	      "SetFileSize shrink + grow");
	CloseFile(h);

	CHECK(CreateDir(S("C:\\NEWDIR")) == ErcOk, "CreateDir");
	CHECK(CreateDir(S("C:\\NEWDIR")) == ErcDupName, "CreateDir twice");
	CHECK(CreateFile(S("C:\\NEWDIR\\X.TXT"), 0) == ErcOk, "file in a new directory");
	CHECK(RenameFile(S("C:\\NEWDIR\\X.TXT"), S("C:\\NEWDIR\\Y.TXT")) == ErcOk, "RenameFile");
	CHECK(OpenFile(S("C:\\NEWDIR\\X.TXT"), ModeRead, 1, &h) == ErcNoSuchFile, "old name gone");
	CHECK(RenameFile(S("C:\\HELLO.TXT"), S("D:\\HELLO.TXT")) == ErcRenameDrv, "rename across volumes");
	CHECK(DeleteDir(S("C:\\NEWDIR")) == ErcDirNotEmpty, "DeleteDir of a non-empty directory");
	CHECK(OpenFile(S("C:\\NEWDIR\\Y.TXT"), ModeRead, 1, &h) == ErcOk && DeleteFile(h) == ErcReadOnly,
	      "DeleteFile needs ModeModify");
	CloseFile(h);
	CHECK(OpenFile(S("C:\\NEWDIR\\Y.TXT"), ModeModify, 1, &h) == ErcOk && DeleteFile(h) == ErcOk,
	      "DeleteFile");
	CHECK(DeleteDir(S("C:\\NEWDIR")) == ErcOk, "DeleteDir (MMURTL's rd hung the system)");
	CHECK(OpenFile(S("C:\\NEWDIR\\Y.TXT"), ModeRead, 1, &h) == ErcNoSuchDir, "deleted directory gone");

	CHECK(CreateDir(S("C:\\MOVEME")) == ErcOk && write_pat(S("C:\\MOVEME\\INSIDE.TXT"), 0, 1) == ErcOk &&
	      OpenFile(S("C:\\MOVEME\\INSIDE.TXT"), ModeModify, 1, &h) == ErcOk &&
	      WriteBytes(h, (const U8 *)"inside\n", 7, &n) == ErcOk, "directory to move");
	CloseFile(h);
	CHECK(RenameFile(S("C:\\MOVEME"), S("C:\\DIR1\\MOVED")) == ErcOk, "move a directory");
	CHECK(read_all(S("C:\\DIR1\\MOVED\\..\\..\\HELLO.TXT"), fbuf, sizeof(fbuf)) == 25,
	      "'..' of the moved directory points to its new parent");

	CHECK(write_pat(S("D:\\D16.TXT"), 3000, 5) == ErcOk && check_pat(S("D:\\D16.TXT"), 3000, 5), "FAT16 write");
	CHECK(write_pat(S("E:\\E12.BIN"), 1UL << 20, 11) == ErcOk && check_pat(S("E:\\E12.BIN"), 1UL << 20, 11),
	      "FAT12 1 MB write (FAT entries across sector boundaries)");
	CHECK(fsys_open_count() == 0, "%u files left open", fsys_open_count());
	kprintf("M7: FAT write ok (create, stream/block write, copy, long names, truncate/extend, "
		"mkdir/rename/delete/rmdir, move dir; FAT32/16/12)\n");
}

static void test_fs_jobs(void)
{
	const struct job *j;
	U32 job;

	CHECK(LoadNewJob(S("C:\\BIN\\HELLO.ELF"), &job) == ErcOk, "LoadNewJob(C:\\BIN\\HELLO.ELF)");
	j = wait_job(job, 500);
	CHECK(j && !j->alive && j->exit_erc == 0, "hello from a file exited with %u", j ? j->exit_erc : 0);
	CHECK(LoadNewJob(S("C:\\HELLO.TXT"), &job) == ErcBadRunFile, "a text file loaded as a program");
	kprintf("M7: LoadNewJob from a file ok (C:\\BIN\\HELLO.ELF ran as job %u)\n", job);

	job = run("fsuser");
	j = wait_job(job, 500);
	CHECK(j && !j->alive && j->exit_erc == 0, "fsuser exited with %u", j ? j->exit_erc : 0);
	CHECK(fsys_open_count() == 0, "the ended job's file was not closed (%u open)", fsys_open_count());
	kprintf("M7: file calls from ring 3 ok (and the file it left open was closed at exit)\n");
	kprintf("DOORS: M7 OK\n");
}

/* ======================================================================== M8 */

/* F: is ext4 made by mkfs.ext4 -T default (tools/mkdisk.py); tools/check_disk.py runs
   e2fsck -fn on it afterwards. */

static void test_ext4_read(void)
{
	struct volume *f = vol_get('F');
	struct dir_entry de;
	U32 h, i;
	U64 n, off = 0;
	bool seen_long = false, seen_big = false;

	CHECK(crc32c_selftest(), "CRC32C self-test (\"123456789\" -> E3069283)");
	CHECK(f && f->kind == VOL_EXT4 && !f->ro, "F: is not a writable ext4 volume");
	CHECK(read_all(S("F:\\HELLO.TXT"), fbuf, sizeof(fbuf)) == 17 && !memcmp(fbuf, "Hello from ext4.\n", 17),
	      "F:\\HELLO.TXT");
	CHECK(read_all(S("F:\\A long file name on ext4.txt"), fbuf, sizeof(fbuf)) == 21, "ext4 long name");
	CHECK(read_all(S("F:/deep/dir/tree/FILE.TXT"), fbuf, sizeof(fbuf)) == 22 &&
	      !memcmp(fbuf, "Deep in an ext4 tree.\n", 22), "ext4 nested directories");
	CHECK(read_all(S("F:\\MixedCase.Txt"), fbuf, sizeof(fbuf)) == 13, "exact-case name");
	/* mkdisk left a committed Linux journal transaction (debugfs) that rewrites JOURNAL.TXT:
	   the mount replayed it */
	CHECK(read_all(S("F:\\JOURNAL.TXT"), fbuf, sizeof(fbuf)) == 14 && !memcmp(fbuf, "after replay!\n", 14),
	      "F:\\JOURNAL.TXT: the journal was not replayed ('%.13s')", fbuf);
	CHECK(OpenFile(S("F:\\mixedcase.txt"), ModeRead, 1, &h) == ErcNoSuchFile,
	      "ext4 names must be case-sensitive");
	CHECK(OpenFile(S("F:\\BIG.BIN"), ModeRead, 1, &h) == ErcOk, "open BIG.BIN");
	for (;;) {
		erc_t erc = ReadBytes(h, fbuf, sizeof(fbuf), &n);

		for (i = 0; i < n; i++)
			CHECK(fbuf[i] == (U8)((off + i) * 7 + (off + i) / 251), "BIG.BIN byte %lu", off + i);
		off += n;
		if (erc)
			break;
	}
	CloseFile(h);
	CHECK(off == 300000, "BIG.BIN: %lu bytes", off);
	CHECK(OpenFile(S("F:\\SPARSE.BIN"), ModeRead, 1, &h) == ErcOk &&
	      ReadBytes(h, fbuf, 4096, &n) == ErcOk && fbuf[0] == 'A' && fbuf[4095] == 'A' &&
	      SetFileLFA(h, 4096 + 500000) == ErcOk && ReadBytes(h, fbuf, 16, &n) == ErcOk &&
	      fbuf[0] == 0 && fbuf[15] == 0 && SetFileLFA(h, 4096 + (1UL << 20)) == ErcOk &&
	      ReadBytes(h, fbuf, 4096, &n) == ErcOk && fbuf[0] == 'B', "SPARSE.BIN (hole reads as zeros)");
	CloseFile(h);
	for (i = 0; ReadDir(S("F:\\"), i, &de) == ErcOk; i++) {
		seen_long |= !strcmp(de.name, "A long file name on ext4.txt") && de.size == 21;
		seen_big |= !strcmp(de.name, "BIG.BIN") && de.size == 300000 && de.mtime > 1700000000;
	}
	CHECK(seen_long && seen_big, "ReadDir F:\\ (%u entries)", i);
	CHECK(ReadDir(S("F:\\"), 999, &de) == ErcNoMatch, "ReadDir past the end");
	CHECK(ReadDir(S("C:\\"), 0, &de) == ErcOk, "ReadDir on FAT too");
	CHECK(GetDirSector(S("F:\\"), fbuf, 512, 0) == ErcNotSupported, "GetDirSector is FAT-only");
	kprintf("M8: ext4 read ok (metadata_csum, 64bit, extents, holes, case-sensitive names, ReadDir)\n");
}

static void test_ext4_write(void)
{
	U32 h, i;
	U64 n, size, k;
	const U64 G = 1UL << 30;

	/* plain file over many blocks */
	CHECK(write_pat(S("F:\\NEW4.BIN"), 100000, 13) == ErcOk && check_pat(S("F:\\NEW4.BIN"), 100000, 13),
	      "ext4: 100000-byte file");
	/* fragmented file: 12 separate extents, more than the inode's 4 -> extent tree depth 1 */
	CHECK(CreateFile(S("F:\\FRAG.BIN"), 0) == ErcOk && OpenFile(S("F:\\FRAG.BIN"), ModeModify, 0, &h) == ErcOk,
	      "FRAG.BIN");
	for (i = 0; i < 12; i++) {
		memset(fbuf, 'a' + (int)i, 4096);
		CHECK(WriteBlock(h, fbuf, 4096, (U64)i * 2 * 4096, &n) == ErcOk, "FRAG.BIN block %u", i);
	}
	CloseFile(h);
	CHECK(OpenFile(S("F:\\FRAG.BIN"), ModeRead, 0, &h) == ErcOk, "reopen FRAG.BIN");
	for (i = 0; i < 12; i++) {
		CHECK(ReadBlock(h, fbuf, 4096, (U64)i * 2 * 4096, &n) == ErcOk && fbuf[0] == 'a' + (int)i &&
		      fbuf[4095] == 'a' + (int)i, "FRAG.BIN data %u", i);
		if (i < 11)
			CHECK(ReadBlock(h, fbuf, 4096, ((U64)i * 2 + 1) * 4096, &n) == ErcOk && !fbuf[0] &&
			      !fbuf[4095], "FRAG.BIN hole %u", i);
	}
	CloseFile(h);

	/* a 5 GB sparse file with data at 4.5 GB: 64-bit sizes and offsets */
	CHECK(CreateFile(S("F:\\HUGE.BIN"), 0) == ErcOk && OpenFile(S("F:\\HUGE.BIN"), ModeModify, 1, &h) == ErcOk,
	      "HUGE.BIN");
	CHECK(SetFileSize(h, 5 * G) == ErcOk && SetFileLFA(h, 4 * G + G / 2) == ErcOk, "5 GB size, LFA 4.5 GB");
	for (k = 0; k < 4096; k++)
		fbuf[k] = patb(k, 17);
	CHECK(WriteBytes(h, fbuf, 4096, &n) == ErcOk && n == 4096, "write at 4.5 GB");
	CHECK(GetFileSize(h, &size) == ErcOk && size == 5 * G, "size %lu", size);
	CloseFile(h);
	CHECK(OpenFile(S("F:\\HUGE.BIN"), ModeRead, 1, &h) == ErcOk && SetFileLFA(h, 4 * G + G / 2) == ErcOk &&
	      ReadBytes(h, fbuf2, 4096, &n) == ErcOk && !memcmp(fbuf, fbuf2, 4096) &&
	      SetFileLFA(h, G) == ErcOk && ReadBytes(h, fbuf2, 64, &n) == ErcOk && !fbuf2[0] && !fbuf2[63],
	      "read back at 4.5 GB, hole at 1 GB");
	CloseFile(h);

	/* names, directories, rename/move, truncate, delete */
	CHECK(write_pat(S("F:\\Long name created by DOORS on ext4.txt"), 0, 1) == ErcOk &&
	      OpenFile(S("F:\\Long name created by DOORS on ext4.txt"), ModeModify, 1, &h) == ErcOk &&
	      WriteBytes(h, (const U8 *)"long ext4\n", 10, &n) == ErcOk, "ext4 long name create");
	CloseFile(h);
	CHECK(CreateFile(S("F:\\Case.txt"), 0) == ErcOk && CreateFile(S("F:\\case.txt"), 0) == ErcOk,
	      "names differing only in case are different files");
	CHECK(CreateDir(S("F:\\NEWDIR4")) == ErcOk && write_pat(S("F:\\NEWDIR4\\X.TXT"), 5000, 7) == ErcOk,
	      "ext4 mkdir + file");
	CHECK(RenameFile(S("F:\\NEWDIR4\\X.TXT"), S("F:\\NEWDIR4\\Y.TXT")) == ErcOk &&
	      check_pat(S("F:\\NEWDIR4\\Y.TXT"), 5000, 7), "ext4 rename");
	CHECK(DeleteDir(S("F:\\NEWDIR4")) == ErcDirNotEmpty, "ext4 DeleteDir non-empty");
	CHECK(OpenFile(S("F:\\NEWDIR4\\Y.TXT"), ModeModify, 1, &h) == ErcOk && DeleteFile(h) == ErcOk,
	      "ext4 DeleteFile");
	CHECK(DeleteDir(S("F:\\NEWDIR4")) == ErcOk, "ext4 DeleteDir");
	CHECK(CreateDir(S("F:\\MOVE4")) == ErcOk && write_pat(S("F:\\MOVE4\\IN.TXT"), 3, 1) == ErcOk &&
	      RenameFile(S("F:\\MOVE4"), S("F:\\deep\\MOVED4")) == ErcOk &&
	      read_all(S("F:\\deep\\MOVED4\\..\\..\\HELLO.TXT"), fbuf, sizeof(fbuf)) == 17,
	      "ext4 move a directory ('..' updated)");
	CHECK(write_pat(S("F:\\TRUNC4.BIN"), 20000, 3) == ErcOk &&
	      OpenFile(S("F:\\TRUNC4.BIN"), ModeModify, 1, &h) == ErcOk && SetFileSize(h, 5000) == ErcOk &&
	      SetFileSize(h, 9000) == ErcOk, "ext4 truncate + sparse extend");
	CloseFile(h);
	{
		static U8 tbuf[9000];

		CHECK(OpenFile(S("F:\\TRUNC4.BIN"), ModeRead, 1, &h) == ErcOk &&
		      ReadBytes(h, tbuf, sizeof(tbuf), &n) == ErcOk && n == 9000, "read TRUNC4");
		CloseFile(h);
		for (k = 0; k < 9000; k++)
			CHECK(tbuf[k] == (k < 5000 ? patb(k, 3) : 0), "TRUNC4.BIN byte %lu", k);
	}
	CHECK(write_pat(S("F:\\GONE.BIN"), 50000, 9) == ErcOk && OpenFile(S("F:\\GONE.BIN"), ModeModify, 1, &h) == ErcOk &&
	      DeleteFile(h) == ErcOk, "ext4 delete a multi-block file");
	CHECK(fsys_open_count() == 0, "%u files left open", fsys_open_count());
	kprintf("M8: ext4 write ok (files, extent tree depth 1, 5 GB sparse file, long names, "
		"mkdir/rename/move/rmdir, truncate, delete)\n");
}

/* Random file operations on F: checked against a model of what every file must hold; the host's
   e2fsck -fn then checks the volume (tools/check_disk.py). The seed changes every run and is printed,
   so repeated `make test` runs cover different sequences. File k holds bytes [0, valid) of pattern
   mul, then zeros up to size. Slots 0-9 live in F:\STRESS, 10-15 in F:\STRESS\Sub dir. */
#define NSLOT 16
static struct { bool used; U64 size, valid; U32 mul; } slot[NSLOT];
static char spath[NSLOT][40];
static U64 rng;

static U64 rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

static U32 slen(U32 k) { return (U32)strlen(spath[k]); }

static void stress_verify(U32 k)
{
	U32 h;
	U64 off, n, i, size;

	CHECK(OpenFile(spath[k], slen(k), ModeRead, 1, &h) == ErcOk, "stress: open %s", spath[k]);
	CHECK(GetFileSize(h, &size) == ErcOk && size == slot[k].size, "stress: %s size %lu, want %lu",
	      spath[k], size, slot[k].size);
	for (off = 0; off < size; off += n) {
		erc_t e = ReadBytes(h, fbuf, sizeof(fbuf), &n);

		CHECK((e == ErcOk || e == ErcEOF) && n, "stress: read %s at %lu: erc %u", spath[k], off, e);
		for (i = 0; i < n; i++)
			CHECK(fbuf[i] == (off + i < slot[k].valid ? patb(off + i, slot[k].mul) : 0),
			      "stress: %s byte %lu (size %lu, valid %lu, seed %lx)", spath[k], off + i,
			      slot[k].size, slot[k].valid, rng);
	}
	CloseFile(h);
}

static void test_ext4_stress(void)
{
	static const char *const what[] = { "write", "write", "truncate/extend", "rename", "delete",
					    "overwrite", "verify", "mkdir/rmdir" };
	U32 ops[8] = { 0 }, i, k, b, h, nops = 400;
	U64 seed, n;
	bool tmpdir = false;

	GetTimerTick(&n);
	rng = seed = (U64)unix_time() * 2654435761UL + n + 1;
	for (k = 0; k < NSLOT; k++) {
		const char *t = k < 10 ? "F:\\STRESS\\Stress file " : "F:\\STRESS\\Sub dir\\file-";
		char *p = spath[k];

		while (*t)
			*p++ = *t++;
		*p++ = (char)('0' + k / 10);
		*p++ = (char)('0' + k % 10);
		if (k < 10) {
			memcpy(p, ".dat", 5);
		} else {
			*p = 0;
		}
	}
	CHECK(CreateDir(S("F:\\STRESS")) == ErcOk && CreateDir(S("F:\\STRESS\\Sub dir")) == ErcOk,
	      "stress: directories");
	for (i = 0; i < nops; i++) {
		U32 op = (U32)(rnd() % 8);

		k = (U32)(rnd() % NSLOT);
		switch (op) {
		case 0: case 1: {		/* (re)write the whole file */
			U64 size = rnd() % 8 ? rnd() % 65536 : rnd() % 300000;
			U32 mul = (U32)(rnd() % 255) | 1;

			CHECK(write_pat(spath[k], slen(k), size, mul) == ErcOk, "stress: write %s", spath[k]);
			if (slot[k].used && slot[k].size > size)
				CHECK(OpenFile(spath[k], slen(k), ModeModify, 1, &h) == ErcOk &&
				      SetFileSize(h, size) == ErcOk && CloseFile(h) == ErcOk, "stress: cut %s", spath[k]);
			slot[k].used = true;
			slot[k].size = slot[k].valid = size;
			slot[k].mul = mul;
			break;
		}
		case 2:				/* truncate or sparse extend */
			if (!slot[k].used)
				continue;
			n = rnd() % (slot[k].size * 2 + 8192);
			CHECK(OpenFile(spath[k], slen(k), ModeModify, 1, &h) == ErcOk && SetFileSize(h, n) == ErcOk &&
			      CloseFile(h) == ErcOk, "stress: SetFileSize(%s, %lu)", spath[k], n);
			slot[k].size = n;
			if (slot[k].valid > n)
				slot[k].valid = n;
			break;
		case 3:				/* rename to a free slot, maybe into the other directory */
			b = (U32)(rnd() % NSLOT);
			if (!slot[k].used || slot[b].used)
				continue;
			CHECK(RenameFile(spath[k], slen(k), spath[b], slen(b)) == ErcOk, "stress: rename %s -> %s",
			      spath[k], spath[b]);
			slot[b] = slot[k];
			slot[k].used = false;
			break;
		case 4:				/* delete */
			if (!slot[k].used)
				continue;
			CHECK(OpenFile(spath[k], slen(k), ModeModify, 1, &h) == ErcOk && DeleteFile(h) == ErcOk,
			      "stress: delete %s", spath[k]);
			slot[k].used = false;
			break;
		case 5: {			/* overwrite/append a chunk that starts inside the valid part */
			U64 off, len = rnd() % 10000 + 1, j, done;

			if (!slot[k].used)
				continue;
			off = rnd() % (slot[k].valid + 1);
			CHECK(OpenFile(spath[k], slen(k), ModeModify, 1, &h) == ErcOk && SetFileLFA(h, off) == ErcOk,
			      "stress: open %s for overwrite", spath[k]);
			for (j = 0; j < len; j += n) {
				n = len - j < sizeof(fbuf) ? len - j : sizeof(fbuf);
				for (b = 0; b < n; b++)
					fbuf[b] = patb(off + j + b, slot[k].mul);
				CHECK(WriteBytes(h, fbuf, n, &done) == ErcOk && done == n, "stress: overwrite %s",
				      spath[k]);
			}
			CloseFile(h);
			if (off + len > slot[k].valid)
				slot[k].valid = off + len;
			if (slot[k].valid > slot[k].size)
				slot[k].size = slot[k].valid;
			break;
		}
		case 6:
			if (!slot[k].used)
				continue;
			stress_verify(k);
			break;
		case 7:
			if (tmpdir)
				CHECK(DeleteDir(S("F:\\STRESS\\Sub dir\\Temp dir")) == ErcOk, "stress: rmdir");
			else
				CHECK(CreateDir(S("F:\\STRESS\\Sub dir\\Temp dir")) == ErcOk, "stress: mkdir");
			tmpdir = !tmpdir;
			break;
		}
		ops[op]++;
	}
	for (k = 0; k < NSLOT; k++) {
		U32 h2;

		if (slot[k].used)
			stress_verify(k);
		else
			CHECK(OpenFile(spath[k], slen(k), ModeRead, 1, &h2) == ErcNoSuchFile, "stress: %s should be gone",
			      spath[k]);
	}
	CHECK(fsys_open_count() == 0, "stress: %u files left open", fsys_open_count());
	kprintf("M8: ext4 stress ok (seed %lx: ", seed);
	for (i = 0; i < 8; i++)
		if (i != 1)
			kprintf("%u %s%s", i ? ops[i] : ops[0] + ops[1], what[i], i < 7 ? ", " : ")\n");
}

/* ======================================================================== M9 */

#define GLOBAL_KEY(k) (0x1100u | (k))		/* Left Ctrl + Left Alt down (KbdState bits 0 and 4) */

/* M9a: virtual screens and the keyboard service. vidtest checks the calls from ring 3 (and reads
   the keys the test runner types); here: job 1's screen, the owner rules, the global keys. */
static void test_video_kbd(void)
{
	const struct job *j;
	U32 job, own, x, y;
	U8 c, a;

	kbd_service_start();
	CHECK(GetVidChar(1, 0, &c, &a) == ErcOk && c == 'D' && a == 0x1F, "status line: '%c' %x", c, a);
	CHECK(GetXY(&x, &y) == ErcOk && y >= 1 && GetVidOwner(&own) == ErcOk && own == 1, "job 1 screen");
	CHECK(PutVidChars(0, 25, "x", 1, 7) == ErcVidParam && SetVidOwner(2) == ErcVidNum &&
	      SetVidOwner(99) == ErcVidNum, "video parameter errors");

	job = run("vidtest");
	j = wait_job(job, 3000);			/* it waits for the keys the runner types */
	CHECK(!j->alive && j->exit_erc == 0, "vidtest: %s, error %u", j->alive ? "still running" : "ended",
	      j->exit_erc);
	CHECK(vid_owner() == 1, "the screen did not go back to job 1 (owner %u)", vid_owner());

	/* Ctrl-Alt-PgDn: next screen. Ctrl-Alt-Del: kill the job on screen, never the OS. */
	job = run("spinner");
	Sleep(10);
	CHECK(SetVidOwner(job) == ErcOk && vid_owner() == job, "SetVidOwner(spinner)");
	kbd_inject(GLOBAL_KEY(0x0C));
	Sleep(5);
	CHECK(vid_owner() == 1, "Ctrl-Alt-PgDn: owner %u, expected 1", vid_owner());
	kbd_inject(GLOBAL_KEY(0x0C));
	Sleep(5);
	CHECK(vid_owner() == job, "Ctrl-Alt-PgDn again: owner %u, expected %u", vid_owner(), job);
	kbd_inject(GLOBAL_KEY(0x7F));
	j = wait_job(job, 200);
	CHECK(!j->alive && j->exit_erc == ErcOpCancel, "Ctrl-Alt-Del did not kill the job on screen");
	CHECK(vid_owner() == 1, "after Ctrl-Alt-Del: owner %u", vid_owner());
	kbd_inject(GLOBAL_KEY(0x7F));			/* on job 1's screen: nothing happens */
	kbd_inject('z');				/* an ordinary key reaches job 1 */
	Sleep(5);
	CHECK(ReadKbd(&x, 0) == ErcOk && (x & 0xFF) == 'z' && ReadKbd(&x, 0) == ErcNoKeyAvail,
	      "key for job 1");
	kprintf("M9: video and keyboard ok (virtual screens, SetVidOwner, ReadKbd, EditLine, "
		"Ctrl-Alt-PgDn, Ctrl-Alt-Del)\n");
}

/* M9b: jobtest runs three programs in one job (Chain, then ExitJob loading the exit run file). */
static void test_job_calls(void)
{
	const struct job *j;
	U32 job, n;
	char s[80];

	job = run("jobtest");
	j = wait_job(job, 3000);
	CHECK(!j->alive && j->exit_erc == 0, "jobtest: %s, error %u", j->alive ? "still running" : "ended",
	      j->exit_erc);
	CHECK(GetPath(job, s, &n) == ErcOk && n == 8, "GetPath of the ended job: %u", n);
	CHECK(fsys_open_count() == 0, "%u files left open after Chain/ExitJob", fsys_open_count());
	CHECK(Chain("jobtest", 7, 0) == ErcBadJobNum, "Chain from the OS job");
	kprintf("M9: job calls ok (paths, user name, SysIn/Out, CMOS clock, Chain, exit run file)\n");
}

/* M9c: the CLI from C:\DOORS\CLI.ELF, driven by keys the test runner types in three batches
   (the key buffer holds 64): md + copy, then an external command found in the system directory
   (hello, run with Chain; the CLI comes back as the exit run file), then dir + exit. */
static void test_cli(void)
{
	const struct job *j;
	U32 job, i;

	CHECK(LoadNewJob(S("C:\\DOORS\\CLI.ELF"), &job) == ErcOk, "LoadNewJob(C:\\DOORS\\CLI.ELF)");
	CHECK(SetVidOwner(job) == ErcOk, "SetVidOwner(CLI)");
	Sleep(50);
	kprintf("M9: type cli1\n");
	for (i = 0; i < 100 && read_all(S("C:\\T9\\H.TXT"), fbuf, sizeof(fbuf)) != 25; i++)
		Sleep(10);
	CHECK(!memcmp(fbuf, "Hello from a FAT32 file.\n", 25), "CLI md + copy: C:\\T9\\H.TXT");
	kprintf("M9: type cli2\n");
	for (i = 0; i < 1000 && job_get(job)->loads < 3; i++)	/* CLI, HELLO.ELF, CLI again */
		Sleep(1);
	CHECK(job_get(job)->loads == 3, "the CLI did not chain to hello and come back (%u loads)",
	      job_get(job)->loads);
	Sleep(50);

	/* the editor (9d): edit a new file with a long path, answer Y to create it, type, Alt-S, Alt-Q */
	kprintf("M9: type ed1\n");
	for (i = 0; i < 1000 && job_get(job)->loads < 4; i++)	/* EDIT.ELF */
		Sleep(1);
	CHECK(job_get(job)->loads == 4, "the CLI did not start the editor");
	Sleep(100);
	kprintf("M9: type ed2\n");				/* Y: create the file */
	Sleep(100);
	kprintf("M9: type ed3\n");				/* text, Alt-S, Alt-Q */
	for (i = 0; i < 1000 && job_get(job)->loads < 5; i++)	/* the CLI again */
		Sleep(1);
	CHECK(job_get(job)->loads == 5, "the editor did not return to the CLI");
	CHECK(read_all(S("C:\\T9\\EDITED_BY_DOORS.TXT"), fbuf, sizeof(fbuf)) == 15 &&
	      !memcmp(fbuf, "Edited in DOORS", 15), "the editor's file");
	Sleep(50);

	kprintf("M9: type cli3\n");
	j = wait_job(job, 1000);
	CHECK(!j->alive && j->exit_erc == 0, "CLI exit: %s, error %u", j->alive ? "still running" : "ended",
	      j->exit_erc);
	CHECK(vid_owner() == 1 && fsys_open_count() == 0, "after the CLI: owner %u, %u files open",
	      vid_owner(), fsys_open_count());
	kprintf("M9: CLI ok (md, copy, external command via Chain and the exit run file, edit, dir, exit)\n");
}

/* M9d: the MMURTL samples. NUMBERS (service) waits for a key through the KEYBOARD service and
   serves two testsvc clients; BusyLoop never sleeps and is killed with Ctrl-Alt-Del. */
static void test_samples(void)
{
	const struct job *j;
	U32 svc, job, i;
	U64 pages;

	pages = pmm_free_pages();
	svc = run("service");
	Sleep(20);
	CHECK(SetVidOwner(svc) == ErcOk, "SetVidOwner(service)");
	for (i = 0; i < 2; i++) {
		job = run("testsvc");
		j = wait_job(job, 300);
		CHECK(!j->alive && j->exit_erc == 0, "testsvc %u: error %u", i, j->exit_erc);
	}
	kbd_inject('x');				/* any key ends the service */
	j = wait_job(svc, 300);
	CHECK(!j->alive && j->exit_erc == ErcOpCancel, "service: %s, error %u",
	      j->alive ? "still running" : "ended", j->exit_erc);

	job = run("busyloop");
	CHECK(SetVidOwner(job) == ErcOk, "SetVidOwner(busyloop)");
	Sleep(150);					/* it sleeps 1 s, then spins */
	CHECK(job_get(job)->alive, "busyloop ended by itself");
	kbd_inject(GLOBAL_KEY(0x7F));			/* Ctrl-Alt-Del */
	j = wait_job(job, 300);
	CHECK(!j->alive && j->exit_erc == ErcOpCancel, "Ctrl-Alt-Del did not end busyloop");
	Sleep(5);
	CHECK(pmm_free_pages() == pages, "pages leaked by the samples: %ld", (long)(pages - pmm_free_pages()));
	kprintf("M9: samples ok (NUMBERS service + KEYBOARD service request, testsvc, BusyLoop + Ctrl-Alt-Del)\n");
}

/* M8b: writing into htree directories, on the second test disk (GPT, ext4 with 1K blocks, G:).
   mkdisk made ONE (150 entries, 1 index level) and TWO (600 entries, 2 levels) with e2fsck -D;
   the names are long (197 bytes, tools/mkdisk.py htree_name) so a few hundred fill many leaves.
   ONE's root fills up and becomes 2 levels; TWO's leaves and index nodes split. The host then runs
   e2fsck -fn, which checks every hash range of the index (tools/check_disk.py). */
static char hpath[MAX_PATH];

static U32 hname(const char *dir, const char *prefix, U32 i, char fill)
{
	U32 n = 0, k;

	memcpy(hpath, "G:\\", 3);
	n = 3;
	memcpy(hpath + n, dir, 3);
	n += 3;
	hpath[n++] = '\\';
	memcpy(hpath + n, prefix, 3);
	n += 3;
	hpath[n++] = (char)('0' + i / 1000);
	hpath[n++] = (char)('0' + i / 100 % 10);
	hpath[n++] = (char)('0' + i / 10 % 10);
	hpath[n++] = (char)('0' + i % 10);
	for (k = 0; k < 190; k++)
		hpath[n++] = fill;
	hpath[n] = 0;
	return n;
}

static U32 dir_count(const char *path, U32 len)
{
	struct dir_entry de;
	U32 i;

	for (i = 0; ReadDir(path, len, i, &de) == ErcOk; i++)
		;
	return i;
}

static void test_htree(void)
{
	U32 i, n, h;
	char to[MAX_PATH];

	CHECK(read_all(S("G:\\README.TXT"), fbuf, sizeof(fbuf)) == 29, "G: (GPT disk 13, 1K-block ext4)");
	CHECK(dir_count(S("G:\\ONE")) == 150 && dir_count(S("G:\\TWO")) == 600, "htree directories as made");
	for (i = 1; i <= 250; i++) {
		erc_t e;

		n = hname("ONE", "new", i, 'n');
		e = CreateFile(hpath, n, 0);
		CHECK(e == ErcOk, "htree create G:\\ONE new%u: error %u", i, e);
	}
	for (i = 1; i <= 200; i++) {
		erc_t e;

		n = hname("TWO", "add", i, 'a');
		e = CreateFile(hpath, n, 0);
		CHECK(e == ErcOk, "htree create G:\\TWO add%u: error %u", i, e);
	}
	for (i = 6; i <= 600; i += 6) {
		n = hname("TWO", "two", i, 't');
		CHECK(OpenFile(hpath, n, ModeModify, 1, &h) == ErcOk && DeleteFile(h) == ErcOk,
		      "htree delete G:\\TWO two%u", i);
	}
	for (i = 1; i <= 20; i++) {
		n = hname("ONE", "new", i, 'n');
		memcpy(to, hpath, n + 1);
		to[3] = 'T';				/* "G:\ONE\..." -> "G:\TWO\..." */
		to[4] = 'W';
		to[5] = 'O';
		CHECK(RenameFile(hpath, n, to, n) == ErcOk, "htree rename new%u into G:\\TWO", i);
	}
	CHECK(dir_count(S("G:\\ONE")) == 380, "G:\\ONE has %u entries, expected 380", dir_count(S("G:\\ONE")));
	CHECK(dir_count(S("G:\\TWO")) == 720, "G:\\TWO has %u entries, expected 720", dir_count(S("G:\\TWO")));
	for (i = 1; i <= 250; i++) {
		n = hname(i <= 20 ? "TWO" : "ONE", "new", i, 'n');
		CHECK(OpenFile(hpath, n, ModeRead, 1, &h) == ErcOk, "htree lookup new%u", i);
		CloseFile(h);
	}
	n = hname("TWO", "two", 6, 't');
	CHECK(OpenFile(hpath, n, ModeRead, 1, &h) == ErcNoSuchFile, "a deleted htree entry is gone");
	kprintf("M8b: htree directory writes ok (leaf and index node splits, 1 -> 2 levels, delete, "
		"rename across directories; GPT disk)\n");
}

/* ROADMAP 1.2: extent trees deeper than one level on write. On G: (1K blocks) a leaf holds 84
   extents and the inode root 4, so one level ends at 336 extents. FRAG2.BIN gets 1000 one-block
   extents with holes between (depth 2), is read back, cut in the middle (the tree shrinks), grown
   again; FRAG3.BIN gets the same and is deleted (every tree block freed). e2fsck checks the
   trees afterwards (tools/check_disk.py). */
static erc_t frag_write(const char *path, U32 plen, U32 from, U32 to)
{
	U32 h, k;
	U64 n;
	erc_t erc;

	erc = CreateFile(path, plen, 0);
	if (erc && erc != ErcDupName)
		return erc;
	if ((erc = OpenFile(path, plen, ModeModify, 0, &h)))
		return erc;
	for (k = from; k < to && !erc; k++) {
		memset(fbuf, (int)(k & 0xFF) | 1, 1024);
		erc = WriteBlock(h, fbuf, 1024, (U64)k * 2048, &n);
	}
	CloseFile(h);
	return erc;
}

static bool frag_check(const char *path, U32 plen, U32 to)
{
	U32 h, k;
	U64 n;
	bool ok = true;

	if (OpenFile(path, plen, ModeRead, 0, &h))
		return false;
	for (k = 0; k < to && ok; k++) {
		ok = ReadBlock(h, fbuf, 1024, (U64)k * 2048, &n) == ErcOk && fbuf[0] == ((k & 0xFF) | 1) &&
		     fbuf[1023] == fbuf[0];
		if (ok && k + 1 < to)
			ok = ReadBlock(h, fbuf, 1024, (U64)k * 2048 + 1024, &n) == ErcOk && !fbuf[0] && !fbuf[1023];
	}
	CloseFile(h);
	return ok;
}

static void test_deep_extents(void)
{
	U32 h;
	U64 size;
	erc_t erc;

	erc = frag_write(S("G:\\FRAG2.BIN"), 0, 400);		/* > 336: depth 2 */
	CHECK(erc == ErcOk, "400 extents: error %u", erc);
	CHECK(frag_check(S("G:\\FRAG2.BIN"), 400), "FRAG2.BIN read back (depth 2)");
	CHECK(OpenFile(S("G:\\FRAG2.BIN"), ModeModify, 0, &h) == ErcOk && SetFileSize(h, 200 * 2048) == ErcOk &&
	      GetFileSize(h, &size) == ErcOk && size == 200 * 2048, "cut FRAG2.BIN at 200 extents");
	CloseFile(h);
	CHECK(frag_check(S("G:\\FRAG2.BIN"), 200), "FRAG2.BIN after the cut");
	erc = frag_write(S("G:\\FRAG2.BIN"), 200, 480);
	CHECK(erc == ErcOk && frag_check(S("G:\\FRAG2.BIN"), 480), "FRAG2.BIN grown to 480 extents: %u", erc);
	erc = frag_write(S("G:\\FRAG3.BIN"), 0, 400);
	CHECK(erc == ErcOk && OpenFile(S("G:\\FRAG3.BIN"), ModeModify, 0, &h) == ErcOk && DeleteFile(h) == ErcOk,
	      "a depth-2 file deleted: %u", erc);
	kprintf("M8b: deep extent trees ok (480 extents at depth 2: split, cut, grow, delete)\n");
}

/* ---- ROADMAP 1.7: full volumes, 0-byte files, maximum name lengths ---- */

static U32 put_num(char *p, U32 n, U32 digits)
{
	U32 i;

	for (i = digits; i > 0; i--, n /= 10)
		p[i - 1] = (char)('0' + n % 10);
	return digits;
}

/* "X:\DIR\Pnnnn.BIN" */
static U32 full_name(char *p, char vol, const char *dir, char pre, U32 i)
{
	U32 n = 0, l = (U32)strlen(dir);

	p[n++] = vol;
	p[n++] = ':';
	p[n++] = '\\';
	memcpy(p + n, dir, l);
	n += l;
	p[n++] = '\\';
	p[n++] = pre;
	n += put_num(p + n, i, 5);
	memcpy(p + n, ".BIN", 5);
	return n + 4;
}

static void space(char vol, struct vol_space *s)
{
	erc_t erc = GetVolSpace(vol, s);

	CHECK(erc == ErcOk && s->unit && s->total, "GetVolSpace(%c): %u", vol, erc);
}

/* Fill vol with 1 MB files until it is full, check what a full disk does, then free it all: the
   free space must come back exactly. Written 64 KB at a time (on ext4 each WriteBytes is one
   journal transaction). */
static U8 fillbuf[65536];

static void fill_volume(char vol)
{
	char dir[8] = "X:\\FULL", p[40];
	struct vol_space s0, s1, s2;
	U32 h, i, n, pl;
	U64 done, size, written = 0, off, k, j;
	erc_t erc = ErcOk;

	dir[0] = vol;
	space(vol, &s0);
	CHECK(CreateDir(dir, 7) == ErcOk, "%c: mkdir FULL", vol);
	for (i = 0; erc == ErcOk; i++) {
		pl = full_name(p, vol, "FULL", 'F', i);
		if ((erc = CreateFile(p, pl, 0)) == ErcDiskFull)
			break;				/* no room left for a directory entry / inode */
		CHECK(erc == ErcOk && OpenFile(p, pl, ModeModify, 1, &h) == ErcOk, "%c: create %s: %u", vol, p, erc);
		for (off = 0, written = 0; off < (1 << 20) && erc == ErcOk; off += k) {
			k = sizeof(fillbuf);
			for (j = 0; j < k; j++)
				fillbuf[j] = patb(off + j, 7 + i % 5);
			done = 0;
			erc = WriteBytes(h, fillbuf, k, &done);
			written += done;
			CHECK(done <= k, "%c: WriteBytes reported %lu of %lu", vol, done, k);
		}
		CHECK(GetFileSize(h, &size) == ErcOk && size == written,
		      "%c: %s holds %lu bytes, but WriteBytes reported %lu (error %u)", vol, p, size, written, erc);
		CloseFile(h);
		CHECK(erc == ErcOk || erc == ErcDiskFull, "%c: write %s: error %u", vol, p, erc);
	}
	n = i;						/* files F00000 .. n-1 exist */
	space(vol, &s1);
	CHECK(s1.free * s1.unit <= 64 * 1024, "%c: full, but %lu units are still free", vol, s1.free);
	/* at 100 %: more data doesn't fit; the files are intact */
	pl = full_name(p, vol, "FULL", 'F', 0);
	CHECK(OpenFile(p, pl, ModeModify, 1, &h) == ErcOk && SetFileLFA(h, ~0UL) == ErcOk, "%c: reopen", vol);
	memset(fbuf, 0x55, sizeof(fbuf));
	erc = WriteBytes(h, fbuf, sizeof(fbuf), &done);
	CHECK(erc == ErcDiskFull || (erc == ErcOk && done == sizeof(fbuf)), "%c: write at full: %u", vol, erc);
	CHECK(erc != ErcOk || s1.free, "%c: a write succeeded with no free space", vol);
	if (erc == ErcDiskFull) {
		CHECK(GetFileSize(h, &size) == ErcOk && size == (1 << 20) + done, "%c: size after a full write", vol);
		SetFileSize(h, 1 << 20);
	} else {
		SetFileSize(h, 1 << 20);
	}
	CloseFile(h);
	/* empty files at 100 %, until the directory needs a block it can't get: on ext4 that create
	   fails after its inode is taken, and must be rolled back (the inode count is checked below) */
	{
		U32 z;

		for (z = 0; z < 2000 && (erc = CreateFile(p, full_name(p, vol, "FULL", 'Z', z), 0)) == ErcOk; z++)
			;
		CHECK(erc == ErcDiskFull, "%c: empty files at 100 %%: %u made, then error %u", vol, z, erc);
		while (z--) {
			pl = full_name(p, vol, "FULL", 'Z', z);
			CHECK(OpenFile(p, pl, ModeModify, 1, &h) == ErcOk && DeleteFile(h) == ErcOk,
			      "%c: delete %s", vol, p);
		}
	}
	pl = full_name(p, vol, "FULL", 'F', 0);
	CHECK(check_pat(p, pl, 1 << 20, 7), "%c: the first file read back", vol);
	pl = full_name(p, vol, "FULL", 'F', n - 1);
	CHECK(check_pat(p, pl, written, 7 + (n - 1) % 5), "%c: the last (partial) file read back", vol);
	for (i = 0; i < n; i++) {
		pl = full_name(p, vol, "FULL", 'F', i);
		CHECK(OpenFile(p, pl, ModeModify, 1, &h) == ErcOk && DeleteFile(h) == ErcOk, "%c: delete %s", vol, p);
	}
	CHECK(DeleteDir(dir, 7) == ErcOk, "%c: rmdir FULL", vol);
	space(vol, &s2);
	CHECK(s2.free == s0.free && s2.inodes_free == s0.inodes_free,
	      "%c: free space not back: %lu -> %lu units, inodes %lu -> %lu", vol, s0.free, s2.free,
	      s0.inodes_free, s2.inodes_free);
	kprintf("1.7: %c: filled to 100%% (%u files, %lu KB) and freed again (tick %lu)\n", vol, n,
		(s0.free - s1.free) * s0.unit / 1024, timer_ticks());
}

/* ext4: empty files until no inode is left */
static void fill_inodes(char vol)
{
	char dir[7] = "X:\\INO", p[40];
	struct vol_space s0, s1, s2;
	U32 i, h, pl;
	erc_t erc = ErcOk;

	dir[0] = vol;
	space(vol, &s0);
	CHECK(CreateDir(dir, 6) == ErcOk, "%c: mkdir INO", vol);
	for (i = 0; (erc = CreateFile(p, full_name(p, vol, "INO", 'I', i), 0)) == ErcOk; i++)
		;
	CHECK(erc == ErcDiskFull, "%c: creating file %u: error %u", vol, i, erc);
	space(vol, &s1);
	CHECK(s1.inodes_free == 0, "%c: CreateFile failed with %lu inodes free", vol, s1.inodes_free);
	while (i--) {
		pl = full_name(p, vol, "INO", 'I', i);
		CHECK(OpenFile(p, pl, ModeModify, 1, &h) == ErcOk && DeleteFile(h) == ErcOk, "%c: delete %s", vol, p);
	}
	CHECK(DeleteDir(dir, 6) == ErcOk, "%c: rmdir INO", vol);
	space(vol, &s2);
	CHECK(s2.free == s0.free && s2.inodes_free == s0.inodes_free, "%c: inodes not back: %lu -> %lu",
	      vol, s0.inodes_free, s2.inodes_free);
	kprintf("1.7: %c: all %lu free inodes used and freed again\n", vol, s0.inodes_free);
}

/* FAT12/16: the fixed root directory fills up */
static void fill_root(char vol)
{
	char p[40];
	struct vol_space s0, s2;
	U32 i, h, pl;
	erc_t erc;

	space(vol, &s0);
	for (i = 0; (erc = CreateFile(p, full_name(p, vol, "", 'R', i) , 0)) == ErcOk; i++)
		;
	CHECK(erc == ErcRootFull && i > 100, "%c: root full after %u files: error %u", vol, i, erc);
	while (i--) {
		pl = full_name(p, vol, "", 'R', i);
		CHECK(OpenFile(p, pl, ModeModify, 1, &h) == ErcOk && DeleteFile(h) == ErcOk, "%c: delete %s", vol, p);
	}
	space(vol, &s2);
	CHECK(s2.free == s0.free, "%c: root fill changed the free space", vol);
	kprintf("1.7: %c: root directory filled (ErcRootFull) and emptied\n", vol);
}

/* 0-byte files and the longest names (255; 256 is refused) */
/* 12 names of 255 characters, the longest FAT (VFAT) and ext4 allow, in the root ("X:\" + 255 =
   258 < MAX_PATH), alike but for their end (FAT: aliases NAME~1 .. NAME~12); a 256th character
   is refused. Created, written, listed and deleted again. */
static void long_names(char vol)
{
	static char p[MAX_PATH + 8];
	struct dir_entry de;
	U32 h, i, k;

	p[0] = vol;
	p[1] = ':';
	p[2] = '\\';
	for (k = 0; k < 12; k++) {
		for (i = 0; i < 255; i++)
			p[3 + i] = (char)('a' + (i + (i > 240 ? k : 0)) % 26);
		CHECK(CreateFile(p, 3 + 255, 0) == ErcOk, "%c: a 255-character name (%u)", vol, k);
		CHECK(write_pat(p, 3 + 255, 100, 3 + k) == ErcOk && check_pat(p, 3 + 255, 100, 3 + k),
		      "%c: 255-character name %u: data", vol, k);
	}
	for (i = 0, k = 0; ReadDir(p, 3, i, &de) == ErcOk; i++)
		k += strlen(de.name) == 255 && de.size == 100;
	CHECK(k == 12, "%c: ReadDir lists %u of the 12 long names", vol, k);
	p[3 + 255] = 'z';
	CHECK(CreateFile(p, 3 + 256, 0) == ErcBadFileSpec, "%c: a 256-character name must be refused", vol);
	for (k = 0; k < 12; k++) {
		for (i = 0; i < 255; i++)
			p[3 + i] = (char)('a' + (i + (i > 240 ? k : 0)) % 26);
		CHECK(OpenFile(p, 3 + 255, ModeModify, 1, &h) == ErcOk && DeleteFile(h) == ErcOk,
		      "%c: delete long name %u", vol, k);
	}
}

static void edge_names(char vol)
{
	static char p[MAX_PATH + 8];
	struct dir_entry de;
	struct vol_space s0, s1, s2;
	U32 h, i;
	U64 done, size;
	char d[] = "X:\\EDGE";

	d[0] = vol;
	space(vol, &s0);
	CHECK(CreateDir(d, 7) == ErcOk, "%c: mkdir EDGE", vol);
	/* a 0-byte file: read, write nothing, size, rename, delete */
	memcpy(p, d, 7);
	memcpy(p + 7, "\\ZERO.BIN", 10);
	CHECK(CreateFile(p, 16, 0) == ErcOk && OpenFile(p, 16, ModeModify, 1, &h) == ErcOk, "%c: zero file", vol);
	CHECK(ReadBytes(h, fbuf, 100, &done) == ErcEOF && done == 0, "%c: read a 0-byte file", vol);
	CHECK(WriteBytes(h, fbuf, 0, &done) == ErcOk && done == 0, "%c: write 0 bytes", vol);
	CHECK(SetFileSize(h, 0) == ErcOk && GetFileSize(h, &size) == ErcOk && size == 0, "%c: size 0", vol);
	CloseFile(h);
	{
		char q[] = "X:\\EDGE\\ZERO2.BIN";

		q[0] = vol;
		CHECK(RenameFile(p, 16, q, sizeof(q) - 1) == ErcOk, "%c: rename a 0-byte file", vol);
		CHECK(ReadDir(d, 7, 0, &de) == ErcOk && de.size == 0 && !strcmp(de.name, "ZERO2.BIN"),
		      "%c: ReadDir of the 0-byte file", vol);
		CHECK(OpenFile(q, sizeof(q) - 1, ModeModify, 1, &h) == ErcOk && DeleteFile(h) == ErcOk,
		      "%c: delete the 0-byte file", vol);
	}
	/* a path longer than MAX_PATH is refused (by FILESYS itself: kernel callers don't check) */
	memcpy(p, d, 7);
	for (i = 7; i < MAX_PATH + 4; i++)
		p[i] = i % 9 == 7 ? '\\' : 'q';
	CHECK(CreateFile(p, MAX_PATH + 4, 0) == ErcBadFileSpec, "%c: a path > MAX_PATH", vol);
	CHECK(DeleteDir(d, 7) == ErcOk, "%c: rmdir EDGE", vol);
	space(vol, &s2);
	CHECK(s2.free == s0.free && s2.inodes_free == s0.inodes_free, "%c: edge tests changed the free space", vol);
	/* twice: the root directory may grow the first time (directories don't shrink, on Linux
	   either), but the second time must reuse the freed entries, with nothing lost */
	long_names(vol);
	space(vol, &s1);
	CHECK(s0.free - s1.free <= 12 * 21 * 32 / s0.unit + 2 && s1.inodes_free == s0.inodes_free,
	      "%c: long names took %lu units", vol, s0.free - s1.free);
	long_names(vol);
	space(vol, &s2);
	CHECK(s2.free == s1.free && s2.inodes_free == s1.inodes_free, "%c: long names: %lu -> %lu units free",
	      vol, s1.free, s2.free);
	kprintf("1.7: %c: 0-byte files, 255-character names (12 alike), 256 and > MAX_PATH refused\n", vol);
}

/* make test: 0-byte files, the longest names, a full root directory, no inodes left, a read-only
   volume. make fulltest also fills each file system and block size to 100 % once: FAT32 C:,
   FAT16 D:, FAT12 E:, ext4 with 1K blocks G:, ext4 with 4K blocks I: (16 MB, 128 inodes:
   tools/mkdisk.py; F: is the same kind but 64 MB). That takes about 3 minutes through ATA PIO. */
static void test_full(bool full)
{
	char vols[] = "CDEGI";
	U32 i;
	U64 t0 = timer_ticks();

	CHECK(CreateFile(S("H:\\NO.TXT"), 0) == ErcReadOnly, "H: (read-only) accepted a new file");
	edge_names('C');
	edge_names('F');
	fill_root('D');
	fill_root('E');
	fill_inodes('I');
	kprintf("1.7: edge cases ok\n");
	if (!full)
		return;
	for (i = 0; vols[i]; i++)
		fill_volume(vols[i]);
	kprintf("1.7: full volumes ok (%lu s)\n", (timer_ticks() - t0) / 100);	/* 10 ms ticks */
}

static void test_ext4_jobs(void)
{
	const struct job *j;
	U32 job;

	CHECK(LoadNewJob(S("F:\\BIN\\HELLO.ELF"), &job) == ErcOk, "LoadNewJob(F:\\BIN\\HELLO.ELF)");
	j = wait_job(job, 500);
	CHECK(j && !j->alive && j->exit_erc == 0, "hello from ext4 exited with %u", j ? j->exit_erc : 0);
	kprintf("M8: LoadNewJob from ext4 ok\n");
	kprintf("DOORS: M8 OK\n");
}

/* Last test: overflow the kernel stack into its guard page. The #PF can't push its frame on
   the dead stack, so the CPU raises #DF, which runs on its own IST stack. */

static void df_handler(struct regs *r)
{
	U8 here;
	U64 sp = (U64)(uintptr_t)&here;

	kprintf("\n*** CPU exception 8: Double fault (expected by the self-test) ***\n");
	dump_regs(r);
	CHECK(sp < (U64)(uintptr_t)kernel_stack_guard || sp > (U64)(uintptr_t)kernel_stack_top,
	      "#DF handler is running on the kernel stack");
	CHECK(r->rsp >= (U64)(uintptr_t)kernel_stack_guard &&
	      r->rsp < (U64)(uintptr_t)kernel_stack_guard + 2 * PAGE_SIZE,
	      "#DF: interrupted RSP %lx is not at the guard page", r->rsp);
	kprintf("M3: stack overflow caught by the guard page; #DF ran on IST1 (RSP=%lx)\n", sp);
	kprintf("DOORS: M3 OK\n");
	qemu_exit(QEMU_EXIT_PASS);
	halt_forever();
}

/* ======================================================================== 3.2 */

/* Each resource is used up in turn. While it is gone, the calls that need it return MMURTL's
   error and a new job fails cleanly; afterwards every count is back and a program runs again.
   Tables that would take long to fill (link blocks, request blocks, tasks, open files) get their
   ceiling lowered to their current size first (gt_limit); pages, exchanges and timers are used up
   for real. */

static struct msg_stats ex_base;
static U64 ex_pages;

static void ex_snapshot(void)
{
	msg_get_stats(&ex_base);
	ex_pages = pmm_free_pages() + gt_pages_total();
}

static void ex_no_job(const char *what, erc_t want)
{
	U32 job = 0;
	erc_t e = LoadNewJob(S("hello"), &job);

	CHECK(e == want, "out of %s: LoadNewJob gave %u, expected %u", what, e, want);
}

static void ex_recovered(const char *what)
{
	const struct job *j;
	struct msg_stats s;
	U64 p;
	U32 job;

	msg_get_stats(&s);
	CHECK(s.exch_free == ex_base.exch_free && s.lb_free == ex_base.lb_free &&
	      s.rqb_free == ex_base.rqb_free && s.tmr_used == ex_base.tmr_used &&
	      s.svc_used == ex_base.svc_used,
	      "after %s: not all back (exch %u/%u, lb %u/%u, rqb %u/%u, tmr %u/%u, svc %u/%u)", what,
	      s.exch_free, ex_base.exch_free, s.lb_free, ex_base.lb_free, s.rqb_free, ex_base.rqb_free,
	      s.tmr_used, ex_base.tmr_used, s.svc_used, ex_base.svc_used);
	p = pmm_free_pages() + gt_pages_total();
	CHECK(p == ex_pages, "after %s: %ld pages not back", what, (long)(ex_pages - p));
	kcheck();
	job = run("hello");
	j = wait_job(job, 500);
	CHECK(j && !j->alive && j->exit_erc == ErcOk, "after %s: hello did not run", what);
}

/* All free memory in OS pages (64 at a time, then fewer), chained through the pages themselves. */
static U32 ex_pages_round(bool check)
{
	struct chunk {
		struct chunk *next;
		U32 n;
	} *head = NULL, *c;
	U32 n = 64, total = 0;
	erc_t e = ErcOk;
	U8 *p;

	while (n) {
		if ((e = AllocOSPage(n, &p))) {
			if (e != ErcNoMem)
				break;
			n /= 2;
			continue;
		}
		c = (struct chunk *)(void *)p;
		c->next = head;
		c->n = n;
		head = c;
		total += n;
	}
	CHECK(e == ErcNoMem, "out of pages: AllocOSPage gave %u, expected ErcNoMem", e);
	if (check) {
		U32 x;

		CHECK(!kmalloc(3 * PAGE_SIZE), "out of pages: kmalloc of 3 pages worked");
		ex_no_job("pages", ErcNoMem);
		CHECK(AllocExch(&x) == ErcOk && DeAllocExch(x) == ErcOk,
		      "out of pages: an exchange (table not full) failed");
	}
	while (head) {
		c = head;
		head = c->next;
		DeAllocPage((U8 *)c, c->n);
	}
	return total;
}

static void test_exhaust(void)
{
	static U32 fh[4096];
	U32 n, x, q, svc, resp, old, job, t;
	U64 m[2], h;
	U32 *xs;
	erc_t e;

	/* pages: a first round makes the OS region's page tables (kept), the second is checked */
	ex_pages_round(false);
	ex_snapshot();
	n = ex_pages_round(true);
	ex_recovered("pages");
	kprintf("3.2: out of pages ok (%u pages taken; AllocOSPage ErcNoMem, kmalloc NULL, LoadNewJob "
		"ErcNoMem; all back)\n", n);

	/* exchanges: every one up to the ceiling */
	xs = kmalloc(nEXCH * sizeof(U32));
	CHECK(xs, "kmalloc for the exchange list");
	for (n = 0; n < nEXCH && !(e = AllocExch(&xs[n])); n++)
		;
	CHECK(e == ErcNoMoreExch, "out of exchanges: AllocExch gave %u", e);
	ex_no_job("exchanges", ErcNoMoreExch);
	x = n;
	CHECK(SendMsg(xs[0], 1, 2) == ErcOk && CheckMsg(xs[0], m) == ErcOk && m[1] == 2,
	      "out of exchanges: messages on an existing one failed");
	while (n)
		DeAllocExch(xs[--n]);
	kfree(xs);
	ex_snapshot();			/* the exchange table grew to its ceiling (kept) */
	ex_recovered("exchanges");
	kprintf("3.2: out of exchanges ok (%u allocated, ErcNoMoreExch; LoadNewJob ErcNoMoreExch)\n",
		x);

	/* link blocks: messages queued where nobody waits */
	old = gt_limit(GT_LB, 0);
	AllocExch(&q);
	for (n = 0; !(e = SendMsg(q, n, 0)); n++)
		;
	CHECK(e == ErcNoMoreLBs, "out of link blocks: SendMsg gave %u", e);
	CHECK(Sleep(2) == ErcOk, "out of link blocks: Sleep failed (a timer wake-up needs none)");
	CHECK(CheckMsg(q, m) == ErcOk && m[0] == 0, "out of link blocks: the first message is lost");
	CHECK(SendMsg(q, 1, 0) == ErcOk, "a freed link block was not reused");
	DeAllocExch(q);			/* drops the rest */
	gt_limit(GT_LB, old);
	ex_recovered("link blocks");
	kprintf("3.2: out of link blocks ok (%u messages queued, ErcNoMoreLBs; Sleep still works)\n", n);

	/* request blocks: requests to a service that doesn't answer yet */
	old = gt_limit(GT_RQB, 0);
	AllocExch(&svc);
	AllocExch(&resp);
	CHECK(RegisterSvc("EXHAUST ", svc) == ErcOk, "RegisterSvc EXHAUST");
	for (n = 0; !(e = Request("EXHAUST ", 1, resp, &h, 0, NULL, 0, NULL, 0, 0, 0, 0)); n++)
		;
	CHECK(e == ErcNoMoreRqBlks, "out of request blocks: Request gave %u", e);
	CHECK(WaitMsg(svc, m) == ErcOk && Respond(m[0], 7) == ErcOk && WaitMsg(resp, m) == ErcOk &&
	      m[1] == 7, "out of request blocks: serving one failed");
	CHECK(Request("EXHAUST ", 1, resp, &h, 0, NULL, 0, NULL, 0, 0, 0, 0) == ErcOk,
	      "a freed request block was not reused");
	UnRegisterSvc("EXHAUST ");
	DeAllocExch(svc);		/* the queued requests fail back to resp */
	CHECK(CheckMsg(resp, m) == ErcOk && m[1] == ErcNoSuchSvc, "a dropped request was not failed");
	DeAllocExch(resp);
	gt_limit(GT_RQB, old);
	ex_recovered("request blocks");
	kprintf("3.2: out of request blocks ok (%u requests, ErcNoMoreRqBlks; the rest failed back "
		"with ErcNoSuchSvc)\n", n);

	/* timers */
	AllocExch(&q);
	for (n = 0; !(e = Alarm(q, 100000)); n++)
		;
	CHECK(e == ErcNoMoreTBs, "out of timers: Alarm gave %u", e);
	CHECK(Sleep(1) == ErcNoMoreTBs, "out of timers: Sleep did not report it");
	KillAlarm(q);
	CHECK(Sleep(1) == ErcOk, "Sleep after KillAlarm");
	DeAllocExch(q);
	ex_recovered("timers");
	kprintf("3.2: out of timers ok (%u alarms, ErcNoMoreTBs; Sleep reports it)\n", n);

	/* tasks: a program starts them until there are no more */
	old = gt_limit(GT_TASKS, 0);
	job = run("taskhog");
	for (t = 0; t < 3000 && !svc_registered("TASKHOG "); t++)
		Sleep(1);
	CHECK(svc_registered("TASKHOG "), "taskhog did not finish starting tasks");
	n = job_task_count(job);
	ex_no_job("tasks", ErcNoMoreTSSs);
	CHECK(KillJob(job) == ErcOk, "KillJob(taskhog)");
	for (t = 0; t < 500 && job_task_count(job); t++)
		Sleep(1);
	Sleep(2);			/* let the zombies be reaped */
	gt_limit(GT_TASKS, old);
	ex_recovered("tasks");
	kprintf("3.2: out of tasks ok (taskhog had %u tasks, then ErcNoMoreTSSs; LoadNewJob too)\n", n);

	/* open files */
	old = gt_limit(GT_FILES, 0);
	for (n = 0; n < ARRAY_SIZE(fh) && !(e = OpenFile(S("C:\\HELLO.TXT"), ModeRead, 1, &fh[n])); n++)
		;
	CHECK(e == ErcNoFreeFCB, "out of files: OpenFile gave %u after %u", e, n);
	x = n;
	while (n)
		CloseFile(fh[--n]);
	CHECK(OpenFile(S("C:\\HELLO.TXT"), ModeRead, 1, &fh[0]) == ErcOk && CloseFile(fh[0]) == ErcOk,
	      "OpenFile after closing");
	gt_limit(GT_FILES, old);
	ex_recovered("open files");
	kprintf("3.2: out of open files ok (%u open, ErcNoFreeFCB)\n", x);
	kprintf("3.2: resource exhaustion ok\n");
}

static __attribute__((noinline)) U64 recurse(U64 n)
{
	volatile U8 buf[256];

	buf[0] = (U8)n;
	return recurse(n + 1) + buf[0];		/* not a tail call */
}

void selftest_run(bool full)
{
	test_timer();
	test_breakpoint();
	test_keyboard();
	test_page_fault();

	test_pmm();
	test_paging();
	test_os_pages();
	test_dma();
	test_spaces();

	test_priority();
	test_round_robin();
	test_context();
	test_task_calls();

	test_exchanges();
	test_wait_switch();
	test_ping_pong();
	test_alarm();
	test_kbd_messages();
	test_request();
	test_cross_job();
	kcheck();			/* 2.4: the kernel's structures agree after each group */

	test_user_hello();
	test_user_badptr();
	test_user_fpu();
	test_user_service();
	test_killjob();
	test_kheap();
	test_many_jobs();
	kcheck();

	test_disk();
	test_fat_read();
	test_fat_write();
	test_fs_jobs();
	test_exhaust();

	test_ext4_read();
	test_ext4_write();
	test_ext4_stress();
	test_htree();
	test_deep_extents();
	test_ext4_jobs();
	test_video_kbd();
	test_job_calls();
	test_cli();
	test_samples();
	test_full(full);
	kcheck();
	kprintf("2.4: kernel invariants ok (scheduler, messaging, page allocator), stack protector on\n");
	cov_dump();			/* make coverage only (empty otherwise) */

	ext4_unmount();				/* the disk is checked by tools/check_disk.py next */
	fat_unmount();
	isr_register(8, df_handler);
	recurse(0);
	panic("stack overflow did not fault");
}
