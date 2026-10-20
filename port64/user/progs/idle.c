/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* idle: waits forever on an exchange nobody sends to; uses no CPU. The 1000-jobs test (ROADMAP 3.1)
   starts many of these and ends them with KillJob. */

#include "doors.h"

int main(void)
{
	U64 m[2];
	U32 exch;

	AllocExch(&exch);
	for (;;)
		WaitMsg(exch, m);
}
