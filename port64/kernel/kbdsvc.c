/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Keyboard service (MMURTL KEYBOARD.ASM KbdTask / ReadKbd).

   The keyboard IRQ translates each key and sends it to this task's exchange. The task:
   - handles global keys (Ctrl and Alt both down), which no program sees, as MMURTL does:
       Ctrl-Alt-PgDn  show the next job's screen (the keyboard follows the screen),
       Ctrl-Alt-Del   kill the job on screen (KillJob; job 1, the OS, is never killed);
   - puts every other key into the buffer of the job that owns the screen (64 keys per job,
     like MMURTL's final buffer), and wakes a task of that job waiting in ReadKbd.

   ReadKbd(pKeyCodeRet, fWait) runs in the caller's task: it takes the next key of the caller's
   own job, waits for one if fWait, or returns ErcNoKeyAvail. Each job has a kernel exchange to
   wait on; a waiting flag avoids queuing a wake-up message per key. */

#include <doors/console.h>
#include <doors/job.h>
#include <doors/msg.h>
#include <doors/rqblk.h>
#include <doors/task.h>
#include <doors/video.h>
#include <doors/x86.h>

#define NKEYS 64

void kbd_set_service(U32 exch);

static struct {
	U32 key[NKEYS];
	U32 in, n;
	U32 exch;			/* wake-up exchange (owned by job 1) */
	bool waiting;
} kq[MAX_JOBS];

static U32 svc_exch;

/* The "KEYBOARD" service (MMURTL's request interface to the keyboard): code 1 reads a key into
   pData1 (4 bytes); dData0 != 0 waits for one. Code 4 gave the keyboard to job dData0; in DOORS the
   keyboard follows the screen, so it only answers. One waiting request per job. */
#define KBD_SVC_READ   1
#define KBD_SVC_ASSIGN 4

static U32 req_exch;
static U64 pending[MAX_JOBS];		/* request waiting for a key (0 = none) */
static U64 stale[MAX_JOBS];		/* request of a job that ended, still to be answered */

static void answer(U64 h, U32 code)
{
	struct RqBlk *r = (struct RqBlk *)(uintptr_t)h;

	if (r->pData1 && r->cbData1 >= sizeof(U32) && !r->orphan)
		*(U32 *)r->pData1 = code;
	Respond(h, ErcOk);
}

static void kbd_req_task(void *unused)
{
	U64 m[2];

	(void)unused;
	for (;;) {
		struct RqBlk *r;
		erc_t erc = WaitMsg(req_exch, m);
		U32 job;
		U64 f;

		if (!erc && !m[0]) {			/* (0, job): a job ended while it waited */
			job = (U32)m[1];
			if (job < MAX_JOBS && stale[job]) {
				Respond(stale[job], ErcOwnerAbort);
				stale[job] = 0;
			}
			continue;
		}
		if (erc) {
			if (erc == ErcBadLinAdd || erc == ErcNoMem || erc == ErcBadJobNum)
				Respond(m[0], erc);	/* couldn't map the caller's buffer */
			continue;
		}
		r = (struct RqBlk *)(uintptr_t)m[0];
		job = r->RqOwnerJob;
		switch (r->ServiceCode) {
		case KBD_SVC_READ:
			f = irq_save();
			if (kq[job].n) {
				U32 code = kq[job].key[kq[job].in];

				kq[job].in = (kq[job].in + 1) % NKEYS;
				kq[job].n--;
				irq_restore(f);
				answer(m[0], code);
			} else if (!r->dData0 || pending[job]) {
				irq_restore(f);
				Respond(m[0], r->dData0 ? ErcDevInUse : ErcNoKeyAvail);
			} else {
				pending[job] = m[0];		/* answered by kbd_task */
				irq_restore(f);
			}
			break;
		case 0:					/* the requester is going away */
		case KBD_SVC_ASSIGN:
			Respond(m[0], ErcOk);
			break;
		default:
			Respond(m[0], ErcBadSvcCode);
		}
	}
}

static void kbd_task(void *unused)
{
	U64 m[2];

	(void)unused;
	for (;;) {
		U32 code, job;
		U64 f;

		if (WaitMsg(svc_exch, m))
			continue;
		code = (U32)m[0];
		if ((code & KBD_CTRL) && (code & KBD_ALT)) {	/* a global key */
			U8 k = code & 0x7F;

			if (k == 0x0C)				/* PgDn (also Num 3) */
				vid_next_owner();
			else if (k == 0x7F && vid_owner() >= FIRST_USER_JOB)	/* Del (also Num .) */
				KillJob(vid_owner());
			continue;
		}
		job = vid_owner();
		f = irq_save();
		if (pending[job]) {				/* a KEYBOARD request waits: answer it */
			U64 h = pending[job];

			pending[job] = 0;
			irq_restore(f);
			answer(h, code);
			continue;
		}
		if (kq[job].n < NKEYS) {			/* full: the key is lost, as in MMURTL */
			kq[job].key[(kq[job].in + kq[job].n) % NKEYS] = code;
			kq[job].n++;
		}
		if (kq[job].waiting) {
			kq[job].waiting = false;
			irq_restore(f);
			SendMsg(kq[job].exch, 1, 0);
			continue;
		}
		irq_restore(f);
	}
}

void kbd_service_start(void)
{
	erc_t erc;
	U32 j;

	if (svc_exch)
		return;
	for (j = 1; j < MAX_JOBS; j++)
		if (AllocExch(&kq[j].exch))
			panic("keyboard: no exchanges");
	if (AllocExch(&svc_exch) || !task_create("keyboard", kbd_task, NULL, 1, 1, &erc))
		panic("keyboard service: %u", erc);
	if (AllocExch(&req_exch) || RegisterSvc("KEYBOARD", req_exch) ||
	    !task_create("kbdreq", kbd_req_task, NULL, 2, 1, &erc))
		panic("KEYBOARD service: %u", erc);
	kbd_set_service(svc_exch);
}

static void flush(U32 job)
{
	U64 m[2];
	U64 f = irq_save();

	kq[job].in = kq[job].n = 0;
	kq[job].waiting = false;
	irq_restore(f);
	if (kq[job].exch)
		while (CheckMsg(kq[job].exch, m) == ErcOk)
			;
}

void kbd_job_new(U32 job)
{
	flush(job);
}

/* Called with interrupts off from the job teardown: a waiting KEYBOARD request is answered later by
   the request task (Respond can switch tasks, so not from here). */
void kbd_job_gone(U32 job)
{
	flush(job);
	if (pending[job]) {
		stale[job] = pending[job];
		pending[job] = 0;
		ISendMsg(req_exch, 0, job);
	}
}

erc_t ReadKbd(U32 *pKeyCodeRet, U32 fWait)
{
	U32 job = current->job;
	U64 m[2];

	if (!svc_exch)
		return ErcNoKeyAvail;
	for (;;) {
		U64 f = irq_save();

		if (kq[job].n) {
			*pKeyCodeRet = kq[job].key[kq[job].in];
			kq[job].in = (kq[job].in + 1) % NKEYS;
			kq[job].n--;
			irq_restore(f);
			return ErcOk;
		}
		if (!fWait) {
			irq_restore(f);
			return ErcNoKeyAvail;
		}
		kq[job].waiting = true;
		irq_restore(f);
		WaitMsg(kq[job].exch, m);
	}
}
