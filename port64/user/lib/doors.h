/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* DOORS user API: the MMURTL system calls with their original names and parameter order
   (MKERNEL.H, MMEMORY.H, MJOB.H, MVID.H). Only types widen: pointers, sizes, messages and
   request handles are 64-bit. Error codes are MMURTL's (doors/erc.h). */
#ifndef DOORS_USER_H
#define DOORS_USER_H

#include <doors/types.h>
#include <doors/dirent.h>
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

/* file system (MFILES.H): 64-bit sizes and file positions (LFA) */
#define ModeRead   0
#define ModeModify 1
erc_t OpenFile(const char *pName, U32 cbName, U32 Mode, U32 fStream, U32 *pdHandleRet);
erc_t CloseFile(U32 dHandle);
erc_t CreateFile(const char *pName, U32 cbName, U32 Attrib);
erc_t DeleteFile(U32 dHandle);
erc_t RenameFile(const char *pCrntName, U32 cbCrntName, const char *pNewName, U32 cbNewName);
erc_t CreateDir(const char *pName, U32 cbName);
erc_t DeleteDir(const char *pName, U32 cbName);
erc_t ReadBytes(U32 dHandle, U8 *pBytesRet, U64 nBytes, U64 *pdBytesRet);
erc_t WriteBytes(U32 dHandle, const U8 *pBytes, U64 nBytes, U64 *pdBytesRet);
erc_t ReadBlock(U32 dHandle, U8 *pBytesRet, U64 nBytes, U64 dLFA, U64 *pdnBytesRet);
erc_t WriteBlock(U32 dHandle, const U8 *pData, U64 nBytes, U64 dLFA, U64 *pdnBytesRet);
erc_t GetFileSize(U32 dHandle, U64 *pdSizeRet);
erc_t SetFileSize(U32 dHandle, U64 dSize);
erc_t GetFileLFA(U32 dHandle, U64 *pdLFARet);
erc_t SetFileLFA(U32 dHandle, U64 dLFA);
erc_t GetDirSector(const char *pPathSpec, U32 cbPathSpec, U8 *pSectorRet, U32 sSectorRet,
		   U32 SectorNum);
erc_t GetSystemDisk(U8 *pDiskRet);
/* DOORS: entry dIndex of a directory on any file system; ErcNoMatch past the last one */
erc_t ReadDir(const char *pPath, U32 cbPath, U32 dIndex, struct dir_entry *pEntryRet);

/* video (MVID.H): each job has its own 80x25 screen; the video owner's is the one shown */
#define BLINK      0x80
#define BLACK      0x00
#define BLUE       0x01
#define GREEN      0x02
#define CYAN       0x03
#define RED        0x04
#define MAGENTA    0x05
#define BROWN      0x06
#define WHITE      0x07
#define GRAY       0x08
#define LTBLUE     0x09
#define LTGREEN    0x0A
#define LTCYAN     0x0B
#define LTRED      0x0C
#define LTMAGENTA  0x0D
#define YELLOW     0x0E
#define BRITEWHITE 0x0F
#define BGBLACK    0x00
#define BGBLUE     0x10
#define BGGREEN    0x20
#define BGCYAN     0x30
#define BGRED      0x40
#define BGMAGENTA  0x50
#define BGBROWN    0x60
#define BGWHITE    0x70
#define ErcVidNum     300
#define ErcVidParam   301
#define ErcEditParam  300
erc_t SetVidOwner(U32 dJobNum);
erc_t GetVidOwner(U32 *pdVidNumRet);
erc_t SetNormVid(U32 dAttr);
erc_t GetNormVid(U32 *pdNormVidRet);
erc_t ClrScr(void);
erc_t TTYOut(const char *pTextOut, U64 ddTextOut, U32 ddAttrib);
erc_t PutVidChars(U32 ddCol, U32 ddLine, const char *pChars, U32 sChars, U32 ddAttrib);
erc_t PutVidAttrs(U32 ddCol, U32 ddLine, U32 sChars, U32 dAttr);
erc_t GetVidChar(U32 ddCol, U32 ddLine, U8 *pCharRet, U8 *pAttrRet);
erc_t SetXY(U32 NewX, U32 NewY);
erc_t GetXY(U32 *pXRet, U32 *pYRet);
erc_t ScrollVid(U32 ddULCol, U32 ddULline, U32 nddCols, U32 nddLines, U32 ddfUp);
erc_t EditLine(U8 *pStr, U32 dCrntLen, U32 dMaxLen, U32 *pdLenRet, U8 *pbExitChar, U32 dEditAttr);
erc_t Beep(void);
erc_t Tone(U32 dHz, U32 dTicks10ms);

/* keyboard (MKBD.H): key in bits 0-7, shift state 8-15, locks 16-23, numeric pad bit 24 */
#define KBD_CTRL   0x0300
#define KBD_SHIFT  0x0C00
#define KBD_ALT    0x3000
#define KBD_NUMPAD 0x01000000
#define ErcNoKeyAvail 700
erc_t ReadKbd(U32 *pKeyCodeRet, U32 fWait);

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
