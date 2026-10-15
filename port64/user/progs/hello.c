/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* hello: the first DOORS user program. Runs in ring 3 in its own job and address space and uses
   the MMURTL calls through SYSCALL. If the kernel's KECHO service is there (make test), it also
   sends it a request with buffers in this job's memory. */

#include "doors.h"

static char reply[64];

int main(void)
{
	U32 job, exch, left;
	U64 t0, t1, h, m[2];
	U8 *page;
	char cmd[80];
	U32 ncmd;

	GetJobNum(&job);
	GetTSSExch(&exch);
	printf("hello: running in ring 3 as job %u (task exchange %u)\n", job, exch);

	GetCmdLine(cmd, &ncmd);
	cmd[ncmd < sizeof(cmd) ? ncmd : sizeof(cmd) - 1] = 0;
	printf("hello: command line '%s'\n", cmd);

	if (AllocPage(2, &page)) {
		printf("hello: AllocPage failed\n");
		return 1;
	}
	page[0] = 'D';
	page[8191] = 'S';
	QueryPages(&left);
	printf("hello: AllocPage(2) -> %p, %u pages free\n", page, left);

	GetTimerTick(&t0);
	Sleep(10);
	GetTimerTick(&t1);
	printf("hello: Sleep(10) took %u ticks\n", (U32)(t1 - t0));

	if (Request("KECHO   ", 1, exch, &h, 1, (U8 *)"ring 3", 7, (U8 *)reply, sizeof(reply),
		    0, 0, 0) == ErcOk && WaitMsg(exch, m) == ErcOk && m[0] == h && m[1] == ErcOk)
		printf("hello: KECHO replied '%s'\n", reply);

	DeAllocPage(page, 2);
	printf("hello: done\n");
	return 0;
}
