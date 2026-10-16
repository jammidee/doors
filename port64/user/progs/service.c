/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* service: MSAMPLES/SERVICE/SERVICE.C (R.A. Burgess, public domain) ported to DOORS. Changes:
   DOORS types and headers (messages and request handles are 64-bit, struct RqBlk from doors.h);
   the number is given out as 4 bytes, as the original's long; printf formats. */

/* Super Simple System Service.
   This is expanded from the sample in the Systems Programming
   chapter to show how to properly deinstall a system service.
   The steps to deinstall are:
   1) UnRegister the Service
   2) Serve all remaining requests at service exchange
   3) Deallocate all resources
   4) Exit
*/

#include "doors.h"
#include <stdio.h>

#define ErcOK         0

const struct RqBlk *pRqBlk;      /* A pointer to a Reqeust Block */
U32 NextNumber = 0;              /* The number to return */
U32 MainExch;                    /* Where we wait for Requests */
U64 Message[2];                  /* The Message with the Request */
U64 rqHndl;                      /* Used for keyboard request */

int main(void)
{
U32 OSError, ErrorToUser, keycode;
U32 *pDataRet;

  OSError = AllocExch(&MainExch);	  /* get an exchange */
  if (OSError)                      /* look for a kernel error */
    ExitJob(OSError);

  OSError = RegisterSvc("NUMBERS ", MainExch);
  if (OSError)                      /* look for a system error */
    ExitJob(OSError);

  SetNormVid(WHITE|BGBLACK);
  ClrScr();

  printf("NUMBERS Service Installed.\r\n");
  printf("ANY valid keystroke will terminate the service.\r\n");

  OSError = Request("KEYBOARD", 1, MainExch, &rqHndl, 0, (U8 *)&keycode,
   					  4, 0, 0, 1, 0, 0);  /* 1 in dData0 = WAIT for key */
  if (OSError)
	  printf("Error on Keyboard Request: %u\r\n", OSError);

  while (1)  		/* WHILE forever (almost...) */
  {
	/* Now we wait for a client or for a keystroke to come back */

    OSError = WaitMsg(MainExch, Message); /* Exch & pointer */

    if (!OSError)
    {
	  if (Message[0] == rqHndl)  /* it was a keystroke and NOT a client */
	  {
		UnRegisterSvc("NUMBERS ");
		while (!CheckMsg(MainExch, Message))
		{
			Respond(Message[0], ErcNoSuchSvc);
		}
		DeAllocExch(MainExch);
		ExitJob(ErcOpCancel);
	  }

      pRqBlk = (const struct RqBlk *)(uintptr_t)Message[0];   /* First QWORD is the RqBlk */

	  if (pRqBlk->ServiceCode == 0)          /* Abort request from OS */
          ErrorToUser = ErcOK;

      else if (pRqBlk->ServiceCode == 1)     /* User Asking for Number */
      {
		 pDataRet = (U32 *)pRqBlk->pData1;
         *pDataRet = NextNumber++;       /* Give them a number */
         ErrorToUser = ErcOK;                 /* Respond with No error */
		printf("NUMBERS Service gave out number: %u.\r\n", NextNumber-1);
      }

      else ErrorToUser = ErcBadSvcCode;       /* Unknown Service code! */

      OSError = Respond(Message[0], ErrorToUser); /* Respond to Request */
    }
  }  /* Loop while(1) */
}
