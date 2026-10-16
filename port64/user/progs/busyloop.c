/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* busyloop: MSAMPLES/BUSYLOOP/BUSYLOOP.C (R.A. Burgess, public domain) ported to DOORS. Changes:
   DOORS types and headers; the tick is printed with %lu (it is 64-bit); i is initialised. */

/* Busy loop demonstrates the preemptive capabilites of MMURTL.
   It doesn't go through the kernel and never sleeps. It is
   a task that is ALWAYS ready to run.
   It runs at the default priority of 25, and rest assured,
   other higher priority tasks will receive messages and be
   queued to run.
   If you run it, you will have to use the CTRL-ALT-DELETE keys
   to kill it.
*/

#include "doors.h"
#include <stdio.h>

U64 tick;

int main(void)
{
long i = 0;

	SetJobName("BusyLoop", 8);
	Sleep(100); /* A small delay before we start eating CPU time */
	while (1)
	{
		GetTimerTick(&tick);
	    printf("%lu \r\n", tick);
		i++;
	}
}
