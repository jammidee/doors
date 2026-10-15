/* Tasks and the scheduler (MMURTL KERNEL.ASM enQueueRdy/deQueueRdy/ChkRdyQ, TMRCODE.ASM
   IntTimer task check, Sleep; see include/doors/task.h for the rules).

   All scheduler state is protected by disabling interrupts (single CPU). Task switches happen
   either in task context (yield, sleep, exit, spawning a higher-priority task) or inside the
   timer interrupt, on the interrupted task's own kernel stack; the IRQ is acknowledged before
   the handler runs (idt.c), so the next timer tick is not held back while another task runs. */

#include <doors/console.h>
#include <doors/cpu.h>
#include <doors/mem.h>
#include <doors/msg.h>
#include <doors/string.h>
#include <doors/task.h>

#define STACK_PAGES 4				/* 16 KB kernel stack per task */
#define STACK_SLOT  (8 * PAGE_SIZE)		/* slot: guard page + stack + unused (also a guard) */

struct task *current;

static struct task tasks[MAX_TASKS];
static struct task *idle;
static struct { struct task *head, *tail; } rdyq[nPRI];
static struct task *zombies;
static U64 switch_tick;
static struct sched_stats st;

extern void switch_to(U64 *save_rsp, U64 new_rsp);
extern void task_trampoline(void);
extern U8 kernel_stack_guard[], kernel_stack_top[];

/* ---- ready queue: one FIFO per priority, 0 = highest ---- */

static void rdy_push(struct task *t)
{
	t->state = TASK_READY;
	t->next = NULL;
	if (rdyq[t->priority].tail)
		rdyq[t->priority].tail->next = t;
	else
		rdyq[t->priority].head = t;
	rdyq[t->priority].tail = t;
	st.ready++;
}

static struct task *rdy_peek(void)
{
	unsigned p;

	for (p = 0; p < nPRI; p++)
		if (rdyq[p].head)
			return rdyq[p].head;
	return NULL;
}

static struct task *rdy_pop(void)
{
	struct task *t = rdy_peek();

	if (t) {
		rdyq[t->priority].head = t->next;
		if (!t->next)
			rdyq[t->priority].tail = NULL;
		t->next = NULL;
		st.ready--;
	}
	return t;
}

/* ---- switching ---- */

/* Free the stacks of tasks that have exited. Runs on another task's stack. */
static void reap(void)
{
	while (zombies) {
		struct task *t = zombies;
		U64 va;

		zombies = t->next;
		for (va = t->stack_lo; va < t->stack_hi; va += PAGE_SIZE) {
			U64 pte = vmm_unmap(&kernel_space, va);

			if (pte)
				pmm_free(pte & PTE_ADDR);
		}
		if (t->own_exch)
			exch_free(t->exch);
		memset(t, 0, sizeof(*t));	/* TASK_FREE */
	}
}

/* Interrupts must be off. `current` must already be queued, sleeping or dead (or be `next`). */
static void switch_task(struct task *next)
{
	struct task *prev = current;

	next->state = TASK_RUNNING;
	if (next == prev)
		return;
	if (prev->state == TASK_RUNNING)
		prev->state = TASK_READY;	/* the idle task: never queued, but no longer running */
	current = next;
	switch_tick = timer_ticks();
	st.switches++;
	tss_set_rsp0(next->stack_hi);
	if (next->space && next->space != current_space)
		vmm_space_switch(next->space);
	switch_to(&prev->rsp, next->rsp);
	reap();					/* back in prev */
}

static void schedule(void)
{
	struct task *n = rdy_pop();

	switch_task(n ? n : idle);
}

/* Give the CPU to a ready task that should run instead of `current`.
   slice: called from the timer, so the 30 ms equal-priority rule applies. */
static void maybe_preempt(bool slice)
{
	struct task *n = rdy_peek();

	if (!n)
		return;
	if (current == idle) {
		switch_task(rdy_pop());
		return;
	}
	if (n->priority < current->priority ||
	    (slice && n->priority == current->priority &&
	     timer_ticks() - switch_tick >= TIME_SLICE)) {
		if (slice)
			st.slices++;
		rdy_push(current);
		switch_task(rdy_pop());
	}
}

void sched_tick(void)
{
	if (!current)
		return;
	current->ticks++;
	if (current == idle)
		st.idle_ticks++;
	msg_timer_tick();		/* Sleep/Alarm timer blocks: ISendMsg when they expire */
	maybe_preempt(true);
}

/* End of an IRQ handler: a task it made ready (ISendMsg) runs now if it strictly outranks the
   interrupted one; equal priorities wait for the time slice, as in MMURTL. */
void sched_irq_exit(void)
{
	if (current)
		maybe_preempt(false);
}

/* The current task waits (it has already been queued on an exchange). Interrupts off. */
void sched_block(void)
{
	current->state = TASK_BLOCKED;
	schedule();
}

/* A waiting task got its message: make it ready. may_switch (task context only): run it now if
   its priority is equal or higher than the caller's (MMURTL SendMsg/Request/Respond). */
void sched_wake(struct task *t, bool may_switch)
{
	rdy_push(t);
	if (may_switch && current != idle && t->priority <= current->priority) {
		rdy_push(current);		/* behind t among equal priorities */
		switch_task(rdy_pop());
	}
}

void sched_yield(void)
{
	U64 f = irq_save();

	if (current != idle)
		rdy_push(current);
	schedule();
	irq_restore(f);
}

/* First code a new task runs (from task_trampoline), still with interrupts off. */
void task_started(void)
{
	reap();
	sti();
}

void task_exit(void)
{
	irq_save();
	current->state = TASK_DEAD;
	current->next = zombies;
	zombies = current;
	schedule();
	panic("task_exit: a dead task was resumed");
}

static void idle_loop(void *arg)
{
	(void)arg;
	for (;;) {
		if (rdy_peek())
			sched_yield();
		__asm__ volatile("sti; hlt");
	}
}

/* Allocate a task slot with its own guarded kernel stack, ready to start at entry(arg).
   Not queued. Interrupts must be off. */
static struct task *alloc_task(const char *name, void (*entry)(void *), void *arg, U8 priority,
			       U32 job, erc_t *erc)
{
	struct task *t = NULL;
	U64 va, *sp;
	unsigned i;

	for (i = 1; i < MAX_TASKS; i++) {	/* slot 0 is the boot task */
		if (tasks[i].state == TASK_FREE) {
			t = &tasks[i];
			break;
		}
	}
	if (!t) {
		*erc = ErcNoMoreTSSs;
		return NULL;
	}
	memset(t, 0, sizeof(*t));
	t->stack_lo = STACK_BASE + i * STACK_SLOT + PAGE_SIZE;		/* page 0 = guard */
	t->stack_hi = t->stack_lo + STACK_PAGES * PAGE_SIZE;
	for (va = t->stack_lo; va < t->stack_hi; va += PAGE_SIZE) {
		U64 pa = pmm_alloc();

		if (!pa || vmm_map(&kernel_space, va, pa, PTE_W | PTE_NX)) {
			if (pa)
				pmm_free(pa);
			t->stack_hi = va;		/* free what was mapped */
			t->next = zombies;
			zombies = t;
			t->state = TASK_DEAD;
			reap();
			*erc = ErcNoMem;
			return NULL;
		}
	}

	/* Initial frame popped by switch_to: r15 r14 r13 r12 rbx rbp, then "return" into
	   task_trampoline with r12 = entry, r13 = arg. RSP ends 16-byte aligned there. */
	sp = (U64 *)(uintptr_t)t->stack_hi - 7;
	sp[0] = 0;
	sp[1] = 0;
	sp[2] = (U64)(uintptr_t)arg;
	sp[3] = (U64)(uintptr_t)entry;
	sp[4] = 0;
	sp[5] = 0;
	sp[6] = (U64)(uintptr_t)task_trampoline;
	t->rsp = (U64)(uintptr_t)sp;

	t->tid = i + 1;
	t->job = job;
	t->priority = priority;
	t->space = job_space(job) ? job_space(job) : &kernel_space;
	memcpy(t->name, name, strlen(name) < sizeof(t->name) ? strlen(name) : sizeof(t->name) - 1);

	/* Every task gets its own exchange (MMURTL TSS_Exch), used by Sleep and GetTSSExch. */
	if (msg_ready()) {
		*erc = exch_alloc_job(job, &t->exch);
		if (*erc) {
			t->state = TASK_DEAD;
			t->next = zombies;
			zombies = t;
			reap();
			return NULL;
		}
		t->own_exch = true;
	}
	t->state = TASK_READY;		/* claimed (also keeps the slot from being reused) */
	*erc = ErcOk;
	return t;
}

struct task *task_create(const char *name, void (*entry)(void *), void *arg, U8 priority,
			 U32 job, erc_t *erc)
{
	U64 f;
	struct task *t;

	if (priority >= nPRI) {
		*erc = ErcBadPriority;
		return NULL;
	}
	f = irq_save();
	t = alloc_task(name, entry, arg, priority, job, erc);
	if (t) {
		rdy_push(t);
		maybe_preempt(false);		/* runs now if it outranks the caller */
	}
	irq_restore(f);
	return t;
}

void sched_init(void)
{
	struct task *boot = &tasks[0];
	erc_t erc;

	boot->tid = 1;
	boot->job = 1;
	boot->priority = 20;
	boot->state = TASK_RUNNING;
	boot->space = &kernel_space;
	boot->stack_lo = (U64)(uintptr_t)kernel_stack_guard + PAGE_SIZE;
	boot->stack_hi = (U64)(uintptr_t)kernel_stack_top;
	memcpy(boot->name, "kmain", 6);
	current = boot;

	idle = alloc_task("idle", idle_loop, NULL, nPRI - 1, 1, &erc);	/* never queued */
	if (!idle)
		panic("sched: cannot create the idle task (erc %u)", erc);
	idle->state = TASK_READY;
	switch_tick = timer_ticks();
}

U32 task_count(void)
{
	U32 n = 0, i;

	for (i = 0; i < MAX_TASKS; i++)
		if (tasks[i].state != TASK_FREE && tasks[i].state != TASK_DEAD)
			n++;
	return n;
}

void sched_get_stats(struct sched_stats *s)
{
	U64 f = irq_save();

	*s = st;
	irq_restore(f);
}

const struct task *task_by_index(U32 i)
{
	return i < MAX_TASKS && tasks[i].state != TASK_FREE ? &tasks[i] : NULL;
}

/* ---- MMURTL calls ---- */

static void spawn_entry(void *fn)
{
	((void (*)(void))fn)();
}

erc_t NewTask(U32 JobNum, U32 CodeSeg, U32 Priority, U32 fDebug, U32 Exch, U64 ESP, U64 EIP)
{
	struct task *t;
	erc_t erc;
	U64 f;

	(void)ESP;			/* OS tasks use their own kernel stack, as in MMURTL */
	if (CodeSeg != SEL_KCODE)
		return ErcNotSupported;	/* user-mode tasks arrive with the loader (M6) */
	if (Priority >= nPRI)
		return ErcBadPriority;
	f = irq_save();
	t = alloc_task("task", spawn_entry, (void *)(uintptr_t)EIP, (U8)Priority, JobNum, &erc);
	if (t) {
		t->fdebug = (U8)fDebug;
		if (Exch) {			/* the caller supplies the task's exchange (MMURTL) */
			if (t->own_exch)
				exch_free(t->exch);
			t->exch = Exch;
			t->own_exch = false;
		}
		rdy_push(t);
		maybe_preempt(false);
	}
	irq_restore(f);
	return erc;
}

erc_t SpawnTask(void (*pEntry)(void), U32 dPriority, U32 fDebug, U8 *pStack, U32 fOSCode)
{
	struct task *t;
	erc_t erc;
	U64 f;

	(void)pStack;			/* kernel tasks always get a guarded kernel stack */
	if (!fOSCode)
		return ErcNotSupported;	/* user-mode tasks arrive with the loader (M6) */
	if (dPriority >= nPRI)
		return ErcBadPriority;
	f = irq_save();
	t = alloc_task("spawned", spawn_entry, (void *)pEntry, (U8)dPriority, current->job, &erc);
	if (t) {
		t->fdebug = (U8)fDebug;
		rdy_push(t);
		maybe_preempt(false);
	}
	irq_restore(f);
	return erc;
}

erc_t SetPriority(U32 bPriority)
{
	U64 f;

	if (bPriority >= nPRI)
		return ErcBadPriority;
	f = irq_save();
	current->priority = (U8)bPriority;
	maybe_preempt(false);		/* someone may outrank us now */
	irq_restore(f);
	return ErcOk;
}

erc_t GetPriority(U8 *pbPriorityRet)
{
	*pbPriorityRet = current->priority;
	return ErcOk;
}

/* Sleep is in kernel/msg.c: a timer block that messages the task's own exchange (MMURTL). */

erc_t GetTimerTick(U64 *pTickRet)
{
	*pTickRet = timer_ticks();
	return ErcOk;
}
