/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* taskhog: starts tasks until the system has no more (ROADMAP 3.2), reports how many and the
   error, registers TASKHOG to say it is done, and waits. The kernel test then checks that a new
   job fails cleanly, and ends this one with KillJob. */

#include "doors.h"

static U32 exch;

static void waiter(void)
{
	U64 m[2];

	for (;;)
		WaitMsg(exch, m);
}

int main(void)
{
	U32 n = 0, done;
	erc_t erc;
	U64 m[2];
	U8 *stack;

	AllocExch(&exch);
	AllocExch(&done);
	for (;;) {
		if ((erc = AllocPage(1, &stack)))
			break;
		if ((erc = SpawnTask(waiter, 26, 0, stack + 4096, 0))) {
			DeAllocPage(stack, 1);
			break;
		}
		n++;
	}
	printf("taskhog: %u tasks, then error %u\n", n, erc);
	RegisterSvc("TASKHOG ", done);
	for (;;)
		WaitMsg(done, m);
}
