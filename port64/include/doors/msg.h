/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* DOORS messaging: exchanges, messages, request blocks and named system services.
   A port of MMURTL's kernel primitives (OSSOURCE/KERNEL.ASM, RQBCODE.ASM, SVCCODE.ASM,
   TMRCODE.ASM) with the same names, parameter order and error codes.

   An exchange is a mailbox. It holds either queued messages or queued waiting tasks, never both.
   A message is two values (MMURTL: two DWORDs; here two U64, because a request handle is a
   pointer). WaitMsg blocks until a message arrives; CheckMsg never blocks.

   Requests carry a request block (RqBlk) from a client to a named service. When the service's
   WaitMsg/CheckMsg receives one, pData1/pData2 are aliased into the service's address space
   (AliasMem) and the RqBlk is updated with the alias addresses; Respond removes the aliases and
   sends (RqHandle, status) to the client's response exchange.

   Message formats:
     SendMsg(e, m1, m2)            -> (m1, m2)
     Request(...)                  -> service gets (RqHandle, 0)
     Respond(h, erc)               -> client gets (h, erc)
     timer (Sleep, Alarm)          -> (0xFFFF..., 0xFFFF...)

   Task switching on send: SendMsg/Request/Respond/MoveRequest switch to the receiving task at
   once if its priority is equal or higher (MMURTL). ISendMsg (for interrupt handlers) never
   switches by itself; on IRQ exit, a STRICTLY higher priority task made ready runs (DOORS policy,
   see task.h). */
#ifndef DOORS_MSG_H
#define DOORS_MSG_H

#include <doors/erc.h>
#include <doors/types.h>

#define nEXCH    1024		/* MMURTL: 256 dynamic + 3 static */
#define nLB      4096		/* link blocks (MMURTL 512) */
#define nRQB     256		/* request blocks (MMURTL 128) */
#define nSVC     64		/* named services (MMURTL 64) */
#define nTmrBlks 64		/* pending Sleep/Alarm timers (MMURTL 32) */

#define TIMER_MSG 0xFFFFFFFFFFFFFFFFUL	/* both halves of a timer message */

#include <doors/rqblk.h>

/* ---- MMURTL calls ---- */
erc_t AllocExch(U32 *pExchRet);
erc_t DeAllocExch(U32 Exch);
erc_t GetTSSExch(U32 *pExchRet);
erc_t SendMsg(U32 Exch, U64 dMsg1, U64 dMsg2);
erc_t ISendMsg(U32 Exch, U64 dMsg1, U64 dMsg2);
erc_t WaitMsg(U32 Exch, U64 *pMsgRet);		/* pMsgRet: 2 x U64 */
erc_t CheckMsg(U32 Exch, U64 *pMsgRet);
erc_t Alarm(U32 dExch, U32 nTicks);
erc_t KillAlarm(U32 dExch);
erc_t Sleep(U32 nTicks);
erc_t RegisterSvc(const char *pName, U32 dExch);	/* 8-byte, null-padded, case sensitive */
erc_t UnRegisterSvc(const char *pName);
erc_t Request(const char *pSvcName, U32 wSvcCode, U32 dRespExch, U64 *pRqHndlRet,
	      U32 dnpSend, U8 *pData1, U64 dcbData1, U8 *pData2, U64 dcbData2,
	      U32 dData0, U32 dData1, U32 dData2);
erc_t Respond(U64 dRqHndl, U32 dStatRet);
erc_t MoveRequest(U64 dRqBlkHndl, U32 DestExch);

/* Kernel mutex the MMURTL way: an exchange holding one token message. lock = WaitMsg (blocks
   while another task holds it), unlock = SendMsg. Task context only. */
struct kmutex {
	U32 exch;
};
void kmutex_init(struct kmutex *m);
void kmutex_lock(struct kmutex *m);
void kmutex_unlock(struct kmutex *m);

/* ---- kernel internal ---- */
enum { LB_DATA, LB_REQ, LB_RESP, LB_REQA };	/* MMURTL DATALB, REQLB, RESPLB, REQLBA (aliased) */

void  msg_init(void);			/* pools; gives the running task its exchange */
void  msg_timer_tick(void);		/* timer IRQ: count down Sleep/Alarm blocks */
erc_t exch_alloc_job(U32 job, U32 *pExchRet);
erc_t exch_check(U32 exch, U32 *owner);	/* ErcOk if allocated */
void  exch_free(U32 exch);		/* task teardown */
bool  msg_ready(void);
erc_t msg_post(U32 exch, U64 lo, U64 hi, U8 type, bool may_switch);

/* kernel/rqb.c */
void  rqb_init(void);
erc_t rqb_received(U64 handle);		/* service side: alias the request's buffers */
void  rqb_dispose(U64 handle);
U32   rqb_free_count(void);
U32   svc_used(U32 i);
U64   rqb_to_user(U64 h);		/* kernel handle -> handle in the RQB_UBASE window */
U64   rqb_from_user(U64 h);
void  rqb_fail(U64 h, U32 erc);		/* answer (h, erc) and free; interrupts off */
void  rqb_job_gone(U32 job);
void  svc_job_gone(U32 job);
bool  svc_registered(const char *name);

/* job teardown helpers in kernel/msg.c (interrupts off) */
struct task;
void  exch_remove_waiter(struct task *t);
void  exch_free_job(U32 job);

struct msg_stats {
	U32 exch_free, lb_free, rqb_free, tmr_used, svc_used;
};
void msg_get_stats(struct msg_stats *s);

#endif
