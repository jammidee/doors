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

void kbd_job_gone(U32 job)
{
	flush(job);
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
