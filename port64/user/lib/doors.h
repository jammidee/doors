/* DOORS user API: the MMURTL system calls with their original names and parameter order
   (MKERNEL.H, MMEMORY.H, MJOB.H, MVID.H). Only types widen: pointers, sizes, messages and
   request handles are 64-bit. Error codes are MMURTL's (doors/erc.h). */
#ifndef DOORS_USER_H
#define DOORS_USER_H

#include <doors/types.h>
#include <doors/erc.h>
#include <doors/rqblk.h>

#define TIMER_MSG 0xFFFFFFFFFFFFFFFFUL	/* both halves of a Sleep/Alarm message */
#define ErcCpuFault 1000		/* a job ended by CPU exception n reports 1000 + n */

/* messaging (MKERNEL.H) */
erc_t AllocExch(U32 *pExchRet);
erc_t DeAllocExch(U32 Exch);
erc_t GetTSSExch(U32 *pExchRet);
erc_t SendMsg(U32 Exch, U64 dMsg1, U64 dMsg2);
erc_t WaitMsg(U32 Exch, U64 *pMsgRet);		/* 2 x U64 */
erc_t CheckMsg(U32 Exch, U64 *pMsgRet);
erc_t Request(const char *pSvcName, U32 wSvcCode, U32 dRespExch, U64 *pRqHndlRet,
	      U32 dnpSend, U8 *pData1, U64 dcbData1, U8 *pData2, U64 dcbData2,
	      U32 dData0, U32 dData1, U32 dData2);
erc_t Respond(U64 dRqHndl, U32 dStatRet);
erc_t MoveRequest(U64 dRqBlkHndl, U32 DestExch);
erc_t RegisterSvc(const char *pName, U32 dExch);
erc_t UnRegisterSvc(const char *pName);

/* tasks and time */
erc_t SpawnTask(void (*pEntry)(void), U32 dPriority, U32 fDebug, U8 *pStack, U32 fOSCode);
erc_t SetPriority(U32 bPriority);
erc_t Sleep(U32 nTicks);
erc_t Alarm(U32 dExch, U32 nTicks);
erc_t KillAlarm(U32 dExch);
erc_t GetTimerTick(U64 *pTickRet);

/* memory (MMEMORY.H) */
erc_t AllocPage(U32 nPages, U8 **ppMemRet);
erc_t DeAllocPage(U8 *pOrigMem, U32 nPages);
erc_t QueryPages(U32 *pdPagesLeft);
erc_t AliasMem(U8 *pMem, U64 dcbMem, U32 dJobNum, U8 **ppAliasRet);
erc_t DeAliasMem(U8 *pAliasMem, U64 dcbAliasBytes, U32 JobNum);

/* jobs (MJOB.H) */
erc_t LoadNewJob(const char *pFileName, U32 cbFileName, U32 *pJobNumRet);
NORETURN void ExitJob(erc_t dError);
erc_t KillJob(U32 JobNum);
erc_t GetJobNum(U32 *pJobNumRet);
erc_t SetJobName(const char *pName, U32 dcbName);
erc_t SetCmdLine(const char *pCmd, U32 dcbCmd);
erc_t GetCmdLine(char *pCmdRet, U32 *pdcbCmdRet);

/* video (MVID.H) */
erc_t TTYOut(const char *pTextOut, U64 ddTextOut, U32 ddAttrib);

/* MMURTL OS helpers, implemented in the library (no system call needed) */
void  CopyData(const void *pSource, void *pDestination, U64 dBytes);
void  CopyDataR(const void *pSource, void *pDestination, U64 dBytes);
void  FillData(void *pDest, U64 cBytes, U8 bFill);
U64   Compare(const void *pS1, const void *pS2, U64 dSize);	/* index of first difference, or dSize */
U64   CompareNCS(const void *pS1, const void *pS2, U64 dSize);	/* same, ignoring case */

/* raw system call (number from doors/syscalls.def, arguments as U64) */
U64   doors_syscall(U64 num, const U64 *args);

/* small C library */
void  *memcpy(void *d, const void *s, size_t n);
void  *memset(void *d, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
