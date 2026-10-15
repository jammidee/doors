/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Request block layout (MMURTL RQB.INC), shared by the kernel and user-mode services.
   A service receives a handle to one of these with each request and reads its fields directly,
   as MMURTL services did. User-mode services see the request block pool through a read-only
   window (RQB_UBASE); their handles point into it. */
#ifndef DOORS_RQBLK_H
#define DOORS_RQBLK_H

#include <doors/types.h>

/* Request block (MMURTL RQB.INC, 64 bytes there; widened here: pointers and sizes 64-bit). */
struct RqBlk {
	U32 ServiceExch;	/* OS fill: exchange of the service (from the name) */
	U32 RespExch;		/* user fill: where the response goes */
	U32 RqOwnerJob;		/* OS fill: job number of the requester */
	U32 ServiceRoute;	/* reserved (network routing) */
	U64 pRqHndlRet;		/* reserved for the user */
	U32 dData0;		/* user fill / service defined (no pointers) */
	U32 dData1;
	U32 dData2;
	U16 ServiceCode;	/* service command number (user fill) */
	U8  npSend;		/* number of send buffers (0..2) */
	U8  npRecv;		/* number of receive buffers (2 - npSend) */
	U8 *pData1;		/* user fill; the service sees its alias */
	U64 cbData1;
	U8 *pData2;
	U64 cbData2;
	/* DOORS private */
	U8 *pOrig1, *pOrig2;	/* the requester's original pointers */
	U32 svc_job;		/* job whose address space holds the aliases (0 = not aliased) */
	U32 inuse;
	U32 orphan;		/* the requesting job has ended: buffers dropped, response discarded */
	struct RqBlk *pNextRQB;
};

#endif
