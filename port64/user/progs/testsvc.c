/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* testsvc: MSAMPLES/SERVICE/TESTSVC.C (R.A. Burgess, public domain) ported to DOORS. Changes:
   DOORS types and headers (64-bit messages and request handle); printf formats. */

/* Test client for the NUMBERS System Service.
   Run this program from another CLI when Service is running
*/

#include "doors.h"
#include <stdio.h>

U32 Number;                     /* The number to return */
U32 Exch;                       /* Where hte service will respond */
U64 Message[2];                 /* The Message from the service */

int main(void)
{
U32 Error;
U64 rqhndl;

	Error = AllocExch(&Exch);	  /* get an exchange */
	if (Error)
		printf("Error %u allocating Exchange.\r\n", Error);

	Error = Request("NUMBERS ", 1, Exch, &rqhndl,
					0, 							/* No Send ptrs */
                    (U8 *)&Number, 4, 0, 0,
                    0,0,0);
	if (Error)
		printf("Error %u from Request.\r\n", Error);

	if (!Error)
	{
		Error = WaitMsg(Exch, Message);
		if (Error)
			printf("Error %u from WaitMsg.\r\n", Error);
		else
		{
			if (Message[1])
				printf("Error %lu from NUMBERS Service.\r\n", Message[1]);
			else
				printf("NUMBERS Service gave out number: %u.\r\n", Number);
		}
	}
	DeAllocExch(Exch);	/* it's nice to do this for the OS */
	return 0;
}
