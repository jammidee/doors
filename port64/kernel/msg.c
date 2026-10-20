/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Exchanges, link blocks, messages and timer blocks (MMURTL KERNEL.ASM messaging half and
   TMRCODE.ASM Sleep/Alarm). See include/doors/msg.h for the model.

   A link block (LB) carries one message on an exchange. An exchange queues either LBs (messages
   waiting for a task) or tasks (waiting for a message). Everything here runs with interrupts
   off while queues change; the tail of WaitMsg/CheckMsg (aliasing request buffers, copying the
   message out) runs with them back on, as in MMURTL. */

#include <doors/console.h>
#include <doors/gtable.h>
#include <doors/mem.h>
#include <doors/msg.h>
#include <doors/string.h>
#include <doors/task.h>
#include <doors/x86.h>

struct lb {
	U64 lo, hi;		/* MMURTL DataLo / DataHi */
	struct lb *next;
	U8 type;		/* LB_DATA / LB_REQ / LB_RESP / LB_REQA */
};

struct exch {
	struct lb *mhead, *mtail;	/* queued messages ... */
	struct task *thead, *ttail;	/* ... or queued waiting tasks */
	U32 owner;			/* job number */
	bool inuse;
};

struct tmrblk {
	bool inuse;
	U32 exch;
	U32 count;
};

/* Exchanges and link blocks are growable tables (ROADMAP 3.1): nEXCH and nLB are ceilings, and
   the free counters count against them (ceiling minus in use), so they don't change when a
   table grows. */
static struct gtable gt_ex, gt_lb;
static struct exch *ex;
static struct lb *lbs, *free_lb;
static U32 n_lb_free, n_exch_free;
static U32 ex_low = 1;			/* no free exchange below this (the search starts here) */
static struct tmrblk tmr[nTmrBlks];
static U32 tmr_used;
static bool up;

/* more link blocks onto the free list; false at the ceiling or without memory */
static bool lb_more(void)
{
	U32 i, old = gt_lb.n;

	if (!gt_grow(&gt_lb, gt_lb.n + 1024))
		return false;
	for (i = gt_lb.n; i-- > old;) {
		lbs[i].next = free_lb;
		free_lb = &lbs[i];
	}
	return true;
}

void msg_init(void)
{
	gt_init(&gt_ex, "exchanges", GT_EXCH, sizeof(struct exch), nEXCH, 1024, 0);
	gt_init(&gt_lb, "link blocks", GT_LB, sizeof(struct lb), nLB, 0, 0);
	ex = (struct exch *)(void *)gt_ex.base;
	lbs = (struct lb *)(void *)gt_lb.base;
	if (!lb_more() || !lb_more() || !lb_more() || !lb_more())
		panic("msg: out of memory for link blocks");
	n_lb_free = nLB;
	ex[0].inuse = true;		/* exchange 0 is never handed out: 0 means "none" */
	ex[0].owner = 1;
	n_exch_free = nEXCH - 1;
	rqb_init();
	up = true;

	if (exch_alloc_job(current->job, &current->exch))	/* the running task (kmain) */
		panic("msg: no exchange for the boot task");
	current->own_exch = true;
}

bool msg_ready(void)
{
	return up;
}

/* ---- link blocks and exchanges (interrupts off) ---- */

static struct lb *lb_alloc(void)
{
	struct lb *lb = free_lb;

	if (!lb && lb_more())
		lb = free_lb;
	if (lb) {
		free_lb = lb->next;
		lb->next = NULL;
		n_lb_free--;
	}
	return lb;
}

static void lb_free(struct lb *lb)
{
	ASSERT(lb >= lbs && lb < lbs + gt_lb.n);	/* not a link block of the pool */
	lb->next = free_lb;
	free_lb = lb;
	n_lb_free++;
}

/* kcheck (interrupts off): the free lists and counters agree; every exchange queues messages or
   waiting tasks, never both, and each queue's tail is its last element. */
const char *msg_check(void)
{
	struct lb *lb;
	U32 n = 0, e, inuse = 0;

	if (!up)
		return NULL;
	for (lb = free_lb; lb; lb = lb->next)
		if (++n > gt_lb.n)
			return "msg: the free link block list has a loop";
	if (n != n_lb_free - (nLB - gt_lb.n))	/* the counter also counts the ones not made yet */
		return "msg: the free link block count doesn't match the free list";
	for (e = 0; e < gt_ex.n; e++) {
		struct exch *x = &ex[e];
		struct lb *last = NULL;
		struct task *t, *tlast = NULL;
		U32 k = 0;

		if (!x->inuse) {
			if (x->mhead || x->thead)
				return "msg: a free exchange has a queue";
			continue;
		}
		inuse++;
		if (x->mhead && x->thead)
			return "msg: an exchange queues both messages and waiting tasks";
		for (lb = x->mhead; lb; lb = lb->next) {
			if (lb < lbs || lb >= lbs + gt_lb.n || ++k > gt_lb.n)
				return "msg: an exchange's message queue is broken";
			last = lb;
		}
		if (x->mtail != last)
			return "msg: an exchange's message tail is not its last message";
		for (t = x->thead, k = 0; t; t = t->next) {
			if (++k > MAX_TASKS)
				return "msg: an exchange's waiting-task queue has a loop";
			tlast = t;
		}
		if (x->ttail != tlast)
			return "msg: an exchange's waiting-task tail is not its last task";
	}
	if (nEXCH - inuse != n_exch_free)
		return "msg: the free exchange count doesn't match the exchanges";
	return NULL;
}

static struct exch *get_exch(U32 e, erc_t *erc)
{
	if (e == 0 || e >= gt_ex.n) {
		*erc = ErcOutOfRange;
		return NULL;
	}
	if (!ex[e].inuse) {
		*erc = ErcNotAlloc;
		return NULL;
	}
	*erc = ErcOk;
	return &ex[e];
}

erc_t exch_check(U32 e, U32 *owner)
{
	U64 f = irq_save();
	erc_t erc;
	struct exch *x = get_exch(e, &erc);

	if (x && owner)
		*owner = x->owner;
	irq_restore(f);
	return erc;
}

erc_t exch_alloc_job(U32 job, U32 *pExchRet)
{
	U64 f = irq_save();
	U32 e;

	for (e = ex_low; ; e++) {
		if (e == gt_ex.n && !gt_grow(&gt_ex, gt_ex.n + 256))
			break;				/* the ceiling, or no memory for the table */
		if (!ex[e].inuse) {
			memset(&ex[e], 0, sizeof(ex[e]));
			ex[e].inuse = true;
			ex[e].owner = job;
			n_exch_free--;
			ex_low = e + 1;
			*pExchRet = e;
			irq_restore(f);
			return ErcOk;
		}
	}
	irq_restore(f);
	return ErcNoMoreExch;
}


/* A task waits at x: it gets the message itself (no link block, so a wake-up such as a timer's
   can't fail for want of link blocks). */
static bool deliver(struct exch *x, U64 lo, U64 hi, U8 type, bool may_switch)
{
	struct task *t = x->thead;

	if (!t)
		return false;
	x->thead = t->next;
	if (!x->thead)
		x->ttail = NULL;
	t->next = NULL;
	t->msg[0] = lo;
	t->msg[1] = hi;
	t->msg_type = type;
	t->wait_erc = ErcOk;
	t->wait_exch = 0;
	sched_wake(t, may_switch);
	return true;
}

/* Send one message (any LB type). Used by SendMsg/ISendMsg here and by rqb.c. */
erc_t msg_post(U32 e, U64 lo, U64 hi, U8 type, bool may_switch)
{
	U64 f = irq_save();
	erc_t erc;
	struct exch *x = get_exch(e, &erc);
	struct lb *lb;

	if (x && !deliver(x, lo, hi, type, may_switch)) {	/* nobody waits: queue it */
		lb = lb_alloc();
		if (!lb) {
			erc = ErcNoMoreLBs;
		} else {
			lb->lo = lo;
			lb->hi = hi;
			lb->type = type;
			lb->next = NULL;
			if (x->mtail)
				x->mtail->next = lb;
			else
				x->mhead = lb;
			x->mtail = lb;
		}
	}
	irq_restore(f);
	return erc;
}

static void kill_alarms(U32 e)
{
	U32 i;

	for (i = 0; i < nTmrBlks; i++)
		if (tmr[i].inuse && tmr[i].exch == e) {
			tmr[i].inuse = false;
			tmr_used--;
		}
}

/* Free an exchange: queued messages are dropped (queued requests go back to the pool),
   waiting tasks wake up with ErcNotAlloc, pending timers for it are cancelled. */
static void dealloc(U32 e)
{
	struct exch *x = &ex[e];

	while (x->mhead) {
		struct lb *lb = x->mhead;

		x->mhead = lb->next;
		if (lb->type == LB_REQ || lb->type == LB_REQA)
			rqb_fail(lb->lo, ErcNoSuchSvc);	/* tell the requester */
		lb_free(lb);
	}
	while (x->thead) {
		struct task *t = x->thead;

		x->thead = t->next;
		t->next = NULL;
		t->wait_erc = ErcNotAlloc;
		t->wait_exch = 0;
		sched_wake(t, false);
	}
	kill_alarms(e);
	memset(x, 0, sizeof(*x));
	if (e < ex_low)
		ex_low = e;
	n_exch_free++;
}

void exch_free(U32 e)
{
	U64 f = irq_save();

	if (e && e < gt_ex.n && ex[e].inuse)
		dealloc(e);
	irq_restore(f);
}

/* ---- MMURTL calls ---- */

erc_t AllocExch(U32 *pExchRet)
{
	if (!pExchRet)
		return ErcNullPtr;
	return exch_alloc_job(current->job, pExchRet);
}

erc_t DeAllocExch(U32 Exch)
{
	U64 f = irq_save();
	erc_t erc;
	struct exch *x = get_exch(Exch, &erc);

	if (x) {
		if (x->owner != current->job && current->job != 1)	/* the OS may free any */
			erc = ErcNotOwner;
		else
			dealloc(Exch);
	}
	irq_restore(f);
	return erc;
}

erc_t GetTSSExch(U32 *pExchRet)
{
	*pExchRet = current->exch;
	return ErcOk;
}

erc_t SendMsg(U32 Exch, U64 dMsg1, U64 dMsg2)
{
	return msg_post(Exch, dMsg1, dMsg2, LB_DATA, true);
}

erc_t ISendMsg(U32 Exch, U64 dMsg1, U64 dMsg2)
{
	return msg_post(Exch, dMsg1, dMsg2, LB_DATA, false);
}

/* Take the first queued message off x into m[2]; false if none. Interrupts off. */
static bool dequeue(struct exch *x, U64 *m, U8 *type)
{
	struct lb *lb = x->mhead;

	if (!lb)
		return false;
	x->mhead = lb->next;
	if (!x->mhead)
		x->mtail = NULL;
	m[0] = lb->lo;
	m[1] = lb->hi;
	*type = lb->type;
	lb_free(lb);
	return true;
}

/* Task context, interrupts on: finish receiving a message. */
static erc_t receive(const U64 *m, U8 type, U64 *pMsgRet)
{
	erc_t erc = ErcOk;

	if (type == LB_REQ)
		erc = rqb_received(m[0]);	/* alias pData1/pData2 into this job */
	pMsgRet[0] = m[0];
	pMsgRet[1] = m[1];
	return erc;
}

erc_t WaitMsg(U32 Exch, U64 *pMsgRet)
{
	U64 f;
	erc_t erc;
	struct exch *x;
	U64 m[2];
	U8 type;

	if (!pMsgRet)
		return ErcNullPtr;
	f = irq_save();
	x = get_exch(Exch, &erc);
	if (!x) {
		irq_restore(f);
		return erc;
	}
	if (!dequeue(x, m, &type)) {
		/* Queue ourselves on the exchange and give up the CPU until a message arrives. */
		current->next = NULL;
		current->wait_exch = Exch;
		if (x->ttail)
			x->ttail->next = current;
		else
			x->thead = current;
		x->ttail = current;
		sched_block();
		if (current->wait_erc) {
			erc = current->wait_erc;
			irq_restore(f);
			return erc;
		}
		m[0] = current->msg[0];
		m[1] = current->msg[1];
		type = current->msg_type;
	}
	irq_restore(f);
	return receive(m, type, pMsgRet);
}

erc_t CheckMsg(U32 Exch, U64 *pMsgRet)
{
	U64 f;
	erc_t erc;
	struct exch *x;
	U64 m[2];
	U8 type;

	if (!pMsgRet)
		return ErcNullPtr;
	f = irq_save();
	x = get_exch(Exch, &erc);
	if (!x) {
		irq_restore(f);
		return erc;
	}
	if (!dequeue(x, m, &type)) {
		irq_restore(f);
		return ErcNoMsg;
	}
	irq_restore(f);
	return receive(m, type, pMsgRet);
}

/* ---- timer blocks: Alarm, KillAlarm, Sleep ---- */

erc_t Alarm(U32 dExch, U32 nTicks)
{
	U64 f = irq_save();
	erc_t erc;
	U32 i;

	if (!get_exch(dExch, &erc)) {
		irq_restore(f);
		return erc;
	}
	for (i = 0; i < nTmrBlks; i++) {
		if (!tmr[i].inuse) {
			tmr[i].inuse = true;
			tmr[i].exch = dExch;
			tmr[i].count = nTicks ? nTicks : 1;
			tmr_used++;
			irq_restore(f);
			return ErcOk;
		}
	}
	irq_restore(f);
	return ErcNoMoreTBs;
}

erc_t KillAlarm(U32 dExch)
{
	U64 f = irq_save();

	kill_alarms(dExch);
	irq_restore(f);
	return ErcOk;
}

/* Timer IRQ: count down; at zero, ISendMsg the timer message (MMURTL IntTimer). */
void msg_timer_tick(void)
{
	U32 i;

	if (!tmr_used)
		return;
	for (i = 0; i < nTmrBlks; i++) {
		if (!tmr[i].inuse || --tmr[i].count)
			continue;
		if (msg_post(tmr[i].exch, TIMER_MSG, TIMER_MSG, LB_DATA, false) == ErcNoMoreLBs) {
			tmr[i].count = 1;		/* out of link blocks: try again next tick */
			continue;
		}
		tmr[i].inuse = false;
		tmr_used--;
	}
}

/* MMURTL Sleep: a timer block on the task's own exchange, then wait for its message. */
erc_t Sleep(U32 nTicks)
{
	U64 msg[2];
	erc_t erc;

	if (!nTicks) {
		sched_yield();
		return ErcOk;
	}
	erc = Alarm(current->exch, nTicks);
	if (erc)
		return erc;
	return WaitMsg(current->exch, msg);
}

void msg_get_stats(struct msg_stats *s)
{
	U64 f = irq_save();
	U32 i, svc = 0;

	for (i = 0; i < nSVC; i++)
		svc += svc_used(i);
	s->exch_free = n_exch_free;
	s->lb_free = n_lb_free;
	s->rqb_free = rqb_free_count();
	s->tmr_used = tmr_used;
	s->svc_used = svc;
	irq_restore(f);
}

/* ---- jobs going away (kernel/job.c), interrupts off ---- */

/* Take a task off the exchange it is waiting at (it is being killed). */
void exch_remove_waiter(struct task *t)
{
	struct exch *x;
	struct task **pp;

	if (!t->wait_exch || t->wait_exch >= gt_ex.n)
		return;
	x = &ex[t->wait_exch];
	for (pp = &x->thead; *pp; pp = &(*pp)->next) {
		if (*pp == t) {
			*pp = t->next;
			if (x->ttail == t) {
				struct task *p = x->thead;

				while (p && p->next)
					p = p->next;
				x->ttail = p;
			}
			break;
		}
	}
	t->next = NULL;
	t->wait_exch = 0;
}

/* Free every exchange owned by `job`. */
void exch_free_job(U32 job)
{
	U32 e;

	for (e = 1; e < gt_ex.n; e++)
		if (ex[e].inuse && ex[e].owner == job)
			dealloc(e);
}

/* ---- kernel mutex (msg.h) ---- */

void kmutex_init(struct kmutex *m)
{
	if (exch_alloc_job(1, &m->exch) || SendMsg(m->exch, 0, 0))
		panic("kmutex: no exchange or link block");
}

void kmutex_lock(struct kmutex *m)
{
	U64 msg[2];

	while (WaitMsg(m->exch, msg))
		;
}

void kmutex_unlock(struct kmutex *m)
{
	ISendMsg(m->exch, 0, 0);	/* never switches here; the waiter runs when its turn comes */
}
