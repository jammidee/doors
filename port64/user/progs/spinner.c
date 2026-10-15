/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* spinner: never ends on its own (a second task waits on an exchange forever, the main task
   spins). Used to test KillJob: every task, exchange and page of the job must go away. */

#include "doors.h"

static U32 exch;
static U8 stack2[4096] __attribute__((aligned(16)));

static void waiter(void)
{
	U64 m[2];

	for (;;)
		WaitMsg(exch, m);
}

int main(void)
{
	volatile U64 n = 0;

	AllocExch(&exch);
	SpawnTask(waiter, 26, 0, stack2 + sizeof(stack2), 0);
	printf("spinner: spinning with 2 tasks\n");
	for (;;)
		n++;
}
