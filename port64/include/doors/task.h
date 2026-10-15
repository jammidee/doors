/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* DOORS tasks and scheduler.

   MMURTL switched tasks in hardware (JMP to a TSS selector) and kept the scheduler state in an
   extended TSS (OSSOURCE/TSS.INC). Long mode has no hardware task switching, so a task here is a
   struct task (the TSS.INC OS fields) plus its own kernel stack, and switching is done in
   software (arch/x86_64/switch.asm).

   Scheduling follows MMURTL (KERNEL.ASM, TMRCODE.ASM):
   - 32 priorities, 0 = highest; one FIFO ready queue per priority.
   - The timer gives the CPU to another ready task of EQUAL or higher priority once the running
     task has had it for 3 ticks (30 ms): round robin within a priority.
   - Deliberate change: a task that becomes ready with a STRICTLY higher priority (woken from
     Sleep, or newly spawned) runs immediately instead of at the next 30 ms check. */
#ifndef DOORS_TASK_H
#define DOORS_TASK_H

#include <doors/types.h>
#include <doors/x86.h>

#define nPRI            32
#define MAX_TASKS       256
#define TIME_SLICE      3		/* ticks (30 ms), as TMRCODE.ASM */
#define DEFAULT_PRIORITY 25		/* MMURTL's default for user tasks (BUSYLOOP.C) */

#include <doors/erc.h>

/* TASK_BLOCKED = waiting at an exchange (WaitMsg; Sleep is a WaitMsg on the task's own exchange). */
enum task_state { TASK_FREE, TASK_READY, TASK_RUNNING, TASK_SLEEPING, TASK_BLOCKED, TASK_DEAD };

struct addr_space;

struct task {
	U64 rsp;			/* saved kernel stack pointer (switch_to) */
	U32 tid;			/* task number (MMURTL TSSNum) */
	U32 job;			/* owning job (MMURTL TSS_pJCB) */
	U8  priority;			/* 0 = highest .. 31 */
	U8  state;			/* enum task_state */
	U8  fdebug;			/* MMURTL TSS_TrapBit: start under the debugger (later) */
	U32 exch;			/* MMURTL TSS_Exch: the task's own exchange (M5) */
	struct addr_space *space;
	U64 stack_lo, stack_hi;		/* kernel stack (stack_lo = lowest mapped byte) */
	void *lb_ret;			/* MMURTL pLBRet: link block delivered to a waiting task */
	erc_t wait_erc;			/* non-zero: the wait ended without a message (exchange freed) */
	bool own_exch;			/* exch was allocated for this task (freed when it ends) */
	U32 wait_exch;			/* TASK_BLOCKED: the exchange it waits at */
	U64 urip, ursp;			/* user tasks: where ring 3 starts */
	bool user;			/* runs in ring 3 */
	U8 fpu[512] __attribute__((aligned(16)));	/* FXSAVE area (x87/SSE state) */
	U64 ticks;			/* CPU ticks used (stats) */
	struct task *next;		/* ready / sleep queue link (MMURTL NextTSS) */
	char name[16];
};

extern struct task *current;

void sched_init(void);			/* the caller becomes task 1 (job 1) */
void sched_tick(void);			/* from the timer interrupt */
void sched_yield(void);
void sched_block(void);			/* interrupts off; current already queued on an exchange */
void sched_wake(struct task *t, bool may_switch);
void sched_irq_exit(void);		/* end of an IRQ handler: preempt for a higher priority */
struct task *task_create(const char *name, void (*entry)(void *), void *arg, U8 priority,
			 U32 job, erc_t *erc);
NORETURN void task_exit(void);
U32  task_count(void);
struct task *task_create_user(const char *name, U32 job, U64 rip, U64 rsp, U8 priority,
			      erc_t *erc);
void sched_kill_job(U32 job);		/* interrupts off; all of job's tasks but the caller */
U32  job_task_count(U32 job);
extern U64 syscall_kstack;
const struct task *task_by_index(U32 i);	/* NULL for a free slot */

struct sched_stats {
	U64 switches;		/* all task switches (MMURTL _nSwitches) */
	U64 slices;		/* preemptive switches by the timer (MMURTL _nSlices) */
	U64 idle_ticks;		/* ticks with nothing to run */
	U32 ready;		/* tasks on the ready queue */
};
void sched_get_stats(struct sched_stats *s);

/* ---- MMURTL task calls (KERNEL.ASM / TMRCODE.ASM), same names and parameter order ---- */
erc_t NewTask(U32 JobNum, U32 CodeSeg, U32 Priority, U32 fDebug, U32 Exch, U64 ESP, U64 EIP);
erc_t SpawnTask(void (*pEntry)(void), U32 dPriority, U32 fDebug, U8 *pStack, U32 fOSCode);
erc_t SetPriority(U32 bPriority);
erc_t GetPriority(U8 *pbPriorityRet);
erc_t GetTimerTick(U64 *pTickRet);

#endif
