/* Milestone self-tests, run with kernel cmdline "test" (make test). Each prints "Mn: ... ok"
   lines to the serial log; the last step exits QEMU with QEMU_EXIT_PASS. Any failure panics,
   which exits QEMU with QEMU_EXIT_FAIL. tools/qemu_test.py drives the steps that need input. */

#include <doors/console.h>
#include <doors/cpu.h>
#include <doors/kernel.h>
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
	CHECK(s.rqb_free == m5_base.rqb_free && !s.svc_used, "request blocks or services leaked");
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
	U32 waited;

	CHECK(a && b, "vmm_space_create failed");
	task_create("up-svc", upcase_service, NULL, 15, 3, &e);
	CHECK(up_ready, "UPCASE service did not start");
	task_create("up-client", upcase_client, NULL, 18, 2, &e);
	for (waited = 0; task_count() > 2 && waited < 300; waited++)	/* kmain + idle left */
		Sleep(1);
	CHECK(up_done, "cross-job request failed or returned wrong data");
	CHECK(up_alias_ok, "service did not see the client's buffer through an alias");
	CHECK(up_move_checked, "MoveRequest to another job's exchange was not refused");
	CHECK(up_alias_gone, "alias still mapped after Respond");
	CHECK(task_count() == 2, "tasks left over: %u", task_count());

	vmm_space_destroy(a);
	vmm_space_destroy(b);
	msg_get_stats(&s);
	CHECK(s.exch_free == m5_base.exch_free && s.lb_free == m5_base.lb_free &&
	      s.rqb_free == m5_base.rqb_free && !s.svc_used && !s.tmr_used,
	      "messaging objects leaked (exch %u/%u, lb %u/%u, rqb %u/%u, svc %u, tmr %u)",
	      s.exch_free, m5_base.exch_free, s.lb_free, m5_base.lb_free, s.rqb_free,
	      m5_base.rqb_free, s.svc_used, s.tmr_used);
	CHECK(pmm_free_pages() == m5_pages, "pages leaked: %ld", (long)(m5_pages - pmm_free_pages()));
	kprintf("M5: cross-job Request ok (job 2 client -> job 3 service, buffers aliased, "
		"MoveRequest to a worker, aliases removed by Respond)\n");
	kprintf("M5: no leaks (exchanges, link blocks, request blocks, services, timers, pages)\n");
	kprintf("DOORS: M5 OK\n");
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

static __attribute__((noinline)) U64 recurse(U64 n)
{
	volatile U8 buf[256];

	buf[0] = (U8)n;
	return recurse(n + 1) + buf[0];		/* not a tail call */
}

void selftest_run(void)
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

	isr_register(8, df_handler);
	recurse(0);
	panic("stack overflow did not fault");
}
