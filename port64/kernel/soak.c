/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Soak test (ROADMAP 2.3, tools/soak.py, `make soak`). Kernel command line:
     soak minutes=M [seed=S]
   For M minutes, in rounds: start a random mix of programs (churn, the main load, plus hello,
   jobtest, fsuser and spinner), let them run 0.5-3 s, and kill about half of the ones still
   running, wherever they are (in a file call, a message wait, a page allocation). kcheck() after
   every round. Every 10 rounds all programs are stopped and the counts must be back where they
   were at the start: free pages, exchanges, link blocks, request blocks, timers, tasks, open
   files. At the end the volumes are marked clean, so the host's fsck can check them.
   Lines: "SOAK: round N ..." on each check, "SOAK PASS ..." or "SOAK FAIL: ...". */

#include <doors/blk.h>
#include <doors/console.h>
#include <doors/cpu.h>
#include <doors/fs.h>
#include <doors/job.h>
#include <doors/kernel.h>
#include <doors/mem.h>
#include <doors/msg.h>
#include <doors/string.h>
#include <doors/task.h>
#include <doors/x86.h>

struct counts {
	U32 pages, tasks, files;
	struct msg_stats m;
};

static U64 rng;

static U32 rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (U32)(rng >> 16);
}

static U32 num_after(const char *cmd, const char *key)
{
	U32 k = (U32)strlen(key), n = 0;

	for (; *cmd; cmd++)
		if (!memcmp(cmd, key, k)) {
			for (cmd += k; *cmd >= '0' && *cmd <= '9'; cmd++)
				n = n * 10 + (U32)(*cmd - '0');
			return n;
		}
	return 0;
}

static void count(struct counts *c)
{
	QueryPages(&c->pages);
	c->tasks = task_count();
	c->files = fsys_open_count();
	msg_get_stats(&c->m);
}

static U32 alive_jobs(void)
{
	U32 j, n = 0;

	for (j = FIRST_USER_JOB; j < MAX_JOBS; j++)
		n += job_get(j) && job_get(j)->alive;
	return n;
}

/* stop every program, and wait until their jobs are gone */
static void stop_all(void)
{
	U32 j, t;

	for (j = FIRST_USER_JOB; j < MAX_JOBS; j++)
		if (job_get(j) && job_get(j)->alive)
			KillJob(j);
	for (t = 0; t < 500 && alive_jobs(); t++)
		Sleep(1);
	Sleep(20);				/* let the services finish what the killed jobs asked */
}

static bool same(const struct counts *a, const struct counts *b)
{
	return a->pages == b->pages && a->tasks == b->tasks && a->files == b->files &&
	       a->m.exch_free == b->m.exch_free && a->m.lb_free == b->m.lb_free &&
	       a->m.rqb_free == b->m.rqb_free && a->m.tmr_used == b->m.tmr_used &&
	       a->m.svc_used == b->m.svc_used;
}

static NORETURN void soak_fail(const char *why, const struct counts *base, const struct counts *now)
{
	kprintf("SOAK FAIL: %s\n", why);
	kprintf("SOAK FAIL: pages %u -> %u, tasks %u -> %u, files %u -> %u, exchanges %u -> %u, "
		"link blocks %u -> %u, request blocks %u -> %u, timers %u -> %u, services %u -> %u\n",
		base->pages, now->pages, base->tasks, now->tasks, base->files, now->files,
		base->m.exch_free, now->m.exch_free, base->m.lb_free, now->m.lb_free,
		base->m.rqb_free, now->m.rqb_free, base->m.tmr_used, now->m.tmr_used,
		base->m.svc_used, now->m.svc_used);
	qemu_exit(QEMU_EXIT_FAIL);
	halt_forever();
}

NORETURN void soak_run(const char *cmdline)
{
	static const char *const progs[] = { "churn", "churn", "churn", "hello", "jobtest", "fsuser",
					     "spinner" };
	U32 minutes = num_after(cmdline, "minutes="), seed = num_after(cmdline, "seed="), round = 0;
	U32 started = 0, killed = 0, i, j;
	U64 end;
	struct counts base, now;

	if (!minutes)
		minutes = 5;
	if (!seed)
		seed = unix_time() ^ (U32)timer_ticks();
	rng = (U64)seed * 0x9E3779B97F4A7C15ULL | 1;
	SetPriority(1);				/* above the programs, which run at 25 */
	Sleep(50);
	count(&base);
	end = timer_ticks() + (U64)minutes * 60 * 100;
	kprintf("SOAK: %u minutes, seed %u; %u pages free\n", minutes, seed, base.pages);
	while (timer_ticks() < end) {
		U32 k = 1 + rnd() % 4;

		round++;
		for (i = 0; i < k; i++) {
			const char *p = progs[rnd() % ARRAY_SIZE(progs)];
			U32 job;

			if (LoadNewJob(p, (U32)strlen(p), &job) == ErcOk) {
				char cmd[16];
				U32 r = rnd(), n = 0;

				do {			/* the program's seed, as text */
					cmd[n++] = (char)('0' + r % 10);
					r /= 10;
				} while (r && n < sizeof(cmd) - 1);
				cmd[n] = 0;
				job_set_cmdline(job, cmd);
				started++;
			}
		}
		Sleep(50 + rnd() % 250);
		for (j = FIRST_USER_JOB; j < MAX_JOBS; j++)
			if (job_get(j) && job_get(j)->alive && rnd() % 2) {
				KillJob(j);
				killed++;
			}
		kcheck();
		if (round % 10 == 0) {
			stop_all();
			kcheck();
			count(&now);
			if (!same(&base, &now))
				soak_fail("resources not back after stopping every program", &base, &now);
			kprintf("SOAK: round %u, %lu s: %u programs started, %u killed; counts back, "
				"invariants ok\n", round, (timer_ticks() - (end - (U64)minutes * 6000)) / 100,
				started, killed);
		}
	}
	stop_all();
	kcheck();
	count(&now);
	if (!same(&base, &now))
		soak_fail("resources not back at the end", &base, &now);
	ext4_unmount();
	fat_unmount();
	kprintf("SOAK PASS (%u minutes, %u rounds, %u programs started, %u killed)\n", minutes, round,
		started, killed);
	qemu_exit(QEMU_EXIT_PASS);
	halt_forever();
}
