/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* DOORS user library: system call stubs (numbers from doors/syscalls.def, so they match MMURTL's
   call gate order), the MMURTL memory helpers and EditLine. The C library is in libc.c. */

#include <stdarg.h>
#include "doors.h"

enum {
#define X(num, name) SYS_##name = num,
#include <doors/syscalls.def>
#undef X
};

/* RAX = number, RDI = &args; result in RAX. The kernel clears RDI, RSI, RDX, R8-R10 on return
   and SYSCALL/SYSRET use RCX and R11. */
U64 doors_syscall(U64 num, const U64 *args)
{
	U64 r = num;
	const U64 *a = args;

	__asm__ volatile("syscall"
			 : "+a"(r), "+D"(a)
			 :
			 : "rcx", "r11", "rdx", "rsi", "r8", "r9", "r10", "memory");
	return r;
}

#define A(x) ((U64)(uintptr_t)(x))
#define CALL(name, ...) ({ const U64 _a[] = { __VA_ARGS__ }; (erc_t)doors_syscall(SYS_##name, _a); })

erc_t AllocExch(U32 *p)			{ return CALL(AllocExch, A(p)); }
erc_t DeAllocExch(U32 e)		{ return CALL(DeAllocExch, e); }
erc_t GetTSSExch(U32 *p)		{ return CALL(GetTSSExch, A(p)); }
erc_t SendMsg(U32 e, U64 m1, U64 m2)	{ return CALL(SendMsg, e, m1, m2); }
erc_t WaitMsg(U32 e, U64 *m)		{ return CALL(WaitMsg, e, A(m)); }
erc_t CheckMsg(U32 e, U64 *m)		{ return CALL(CheckMsg, e, A(m)); }
erc_t Respond(U64 h, U32 erc)		{ return CALL(Respond, h, erc); }
erc_t MoveRequest(U64 h, U32 e)		{ return CALL(MoveRequest, h, e); }
erc_t RegisterSvc(const char *n, U32 e)	{ return CALL(RegisterSvc, A(n), e); }
erc_t UnRegisterSvc(const char *n)	{ return CALL(UnRegisterSvc, A(n)); }

erc_t Request(const char *pSvcName, U32 wSvcCode, U32 dRespExch, U64 *pRqHndlRet,
	      U32 dnpSend, U8 *pData1, U64 dcbData1, U8 *pData2, U64 dcbData2,
	      U32 dData0, U32 dData1, U32 dData2)
{
	return CALL(Request, A(pSvcName), wSvcCode, dRespExch, A(pRqHndlRet), dnpSend,
		    A(pData1), dcbData1, A(pData2), dcbData2, dData0, dData1, dData2);
}

erc_t SpawnTask(void (*pEntry)(void), U32 dPriority, U32 fDebug, U8 *pStack, U32 fOSCode)
{
	return CALL(SpawnTask, A(pEntry), dPriority, fDebug, A(pStack), fOSCode);
}

erc_t SetPriority(U32 p)		{ return CALL(SetPriority, p); }
erc_t Sleep(U32 n)			{ return CALL(Sleep, n); }
erc_t Alarm(U32 e, U32 n)		{ return CALL(Alarm, e, n); }
erc_t KillAlarm(U32 e)			{ return CALL(KillAlarm, e); }
erc_t GetTimerTick(U64 *p)		{ return CALL(GetTimerTick, A(p)); }

erc_t AllocPage(U32 n, U8 **pp)		{ return CALL(AllocPage, n, A(pp)); }
erc_t DeAllocPage(U8 *p, U32 n)		{ return CALL(DeAllocPage, A(p), n); }
erc_t QueryPages(U32 *p)		{ return CALL(QueryPages, A(p)); }
erc_t AliasMem(U8 *p, U64 cb, U32 j, U8 **pp)	{ return CALL(AliasMem, A(p), cb, j, A(pp)); }
erc_t DeAliasMem(U8 *p, U64 cb, U32 j)	{ return CALL(DeAliasMem, A(p), cb, j); }

erc_t LoadNewJob(const char *f, U32 cb, U32 *pj)	{ return CALL(LoadNewJob, A(f), cb, A(pj)); }
erc_t KillJob(U32 j)			{ return CALL(KillJob, j); }
erc_t GetJobNum(U32 *p)			{ return CALL(GetJobNum, A(p)); }
erc_t SetJobName(const char *n, U32 cb)	{ return CALL(SetJobName, A(n), cb); }
erc_t SetCmdLine(const char *c, U32 cb)	{ return CALL(SetCmdLine, A(c), cb); }
erc_t GetCmdLine(char *c, U32 *pcb)	{ return CALL(GetCmdLine, A(c), A(pcb)); }
erc_t Chain(const char *f, U32 cb, erc_t e)	{ return CALL(Chain, A(f), cb, (U64)e); }
erc_t SetPath(const char *p, U32 cb)	{ return CALL(SetPath, A(p), cb); }
erc_t GetPath(U32 j, char *p, U32 *pcb)	{ return CALL(GetPath, j, A(p), A(pcb)); }
erc_t SetUserName(const char *p, U32 cb)	{ return CALL(SetUserName, A(p), cb); }
erc_t GetUserName(char *p, U32 *pcb)	{ return CALL(GetUserName, A(p), A(pcb)); }
erc_t SetExitJob(const char *p, U32 cb)	{ return CALL(SetExitJob, A(p), cb); }
erc_t GetExitJob(char *p, U32 *pcb)	{ return CALL(GetExitJob, A(p), A(pcb)); }
erc_t SetSysIn(const char *p, U32 cb)	{ return CALL(SetSysIn, A(p), cb); }
erc_t GetSysIn(char *p, U32 *pcb)	{ return CALL(GetSysIn, A(p), A(pcb)); }
erc_t SetSysOut(const char *p, U32 cb)	{ return CALL(SetSysOut, A(p), cb); }
erc_t GetSysOut(char *p, U32 *pcb)	{ return CALL(GetSysOut, A(p), A(pcb)); }
erc_t GetCMOSTime(U32 *p)		{ return CALL(GetCMOSTime, A(p)); }
erc_t GetCMOSDate(U32 *p)		{ return CALL(GetCMOSDate, A(p)); }

void ExitJob(erc_t e)
{
	CALL(ExitJob, (U64)e);
	for (;;)
		;
}

erc_t TTYOut(const char *p, U64 n, U32 attr)	{ return CALL(TTYOut, A(p), n, attr); }
erc_t SetVidOwner(U32 j)			{ return CALL(SetVidOwner, j); }
erc_t GetVidOwner(U32 *p)			{ return CALL(GetVidOwner, A(p)); }
erc_t SetNormVid(U32 a)				{ return CALL(SetNormVid, a); }
erc_t GetNormVid(U32 *p)			{ return CALL(GetNormVid, A(p)); }
erc_t ClrScr(void)				{ return (erc_t)doors_syscall(SYS_ClrScr, NULL); }
erc_t PutVidChars(U32 x, U32 y, const char *p, U32 n, U32 a)
						{ return CALL(PutVidChars, x, y, A(p), n, a); }
erc_t PutVidAttrs(U32 x, U32 y, U32 n, U32 a)	{ return CALL(PutVidAttrs, x, y, n, a); }
erc_t GetVidChar(U32 x, U32 y, U8 *c, U8 *a)	{ return CALL(GetVidChar, x, y, A(c), A(a)); }
erc_t SetXY(U32 x, U32 y)			{ return CALL(SetXY, x, y); }
erc_t GetXY(U32 *x, U32 *y)			{ return CALL(GetXY, A(x), A(y)); }
erc_t ScrollVid(U32 x, U32 y, U32 nx, U32 ny, U32 up)
						{ return CALL(ScrollVid, x, y, nx, ny, up); }
erc_t Beep(void)				{ return (erc_t)doors_syscall(SYS_Beep, NULL); }
erc_t Tone(U32 hz, U32 t)			{ return CALL(Tone, hz, t); }
erc_t ReadKbd(U32 *p, U32 fWait)		{ return CALL(ReadKbd, A(p), fWait); }

/* EditLine (MMURTL VIDEO.ASM __EditLine): edit pStr in place at the cursor position. Backspace
   and Left delete the last character, 20h-7Eh are typed in, any other key (Enter, Esc, ...) ends
   the edit and is returned in *pbExitChar. Lengths are at most 80. The text is shown in
   dEditAttr while editing and in the normal attribute after. */
erc_t EditLine(U8 *pStr, U32 dCrntLen, U32 dMaxLen, U32 *pdLenRet, U8 *pbExitChar, U32 dEditAttr)
{
	U32 x0, y0, key, norm, i;
	erc_t erc;

	if (dCrntLen > 80 || dMaxLen > 80 || dCrntLen > dMaxLen)
		return ErcEditParam;
	if ((erc = GetXY(&x0, &y0)))
		return erc;
	for (i = dCrntLen; i < dMaxLen; i++)
		pStr[i] = ' ';
	for (;;) {
		U8 k;

		if ((erc = SetXY(x0 + dCrntLen, y0)) ||
		    (erc = PutVidChars(x0, y0, (const char *)pStr, dMaxLen, dEditAttr)))
			return erc;
		do {
			if ((erc = ReadKbd(&key, 1)))
				return erc;
			k = key & 0x7F;
		} while (!k);
		if (k == 0x08 || k == 0x03) {			/* BackSpace, Left (also Num-Left) */
			if (dCrntLen)
				pStr[--dCrntLen] = ' ';
			continue;
		}
		if (k < 0x20 || k > 0x7E)			/* Enter, Esc, function keys... */
			break;
		if (dCrntLen < dMaxLen)		/* MMURTL wrote one byte past a full line here */
			pStr[dCrntLen++] = k;
	}
	*pbExitChar = (U8)(key & 0x7F);
	*pdLenRet = dCrntLen;
	GetNormVid(&norm);
	PutVidChars(x0, y0, (const char *)pStr, dMaxLen, norm);
	return ErcOk;
}

/* ---- MMURTL helpers in the library ---- */

void CopyData(const void *s, void *d, U64 n)	{ memcpy(d, s, n); }

void CopyDataR(const void *s, void *d, U64 n)	/* copies from the end (overlapping moves) */
{
	const U8 *ps = s;
	U8 *pd = d;

	while (n--)
		pd[n] = ps[n];
}

void FillData(void *d, U64 n, U8 b)		{ memset(d, b, n); }

S64 Compare(const void *a, const void *b, U64 n)
{
	const U8 *x = a, *y = b;
	U64 i;

	for (i = 0; i < n && x[i] == y[i]; i++)
		;
	return i == n ? -1 : (S64)i;		/* MMURTL: -1 when all bytes match */
}

static U8 lower(U8 c) { return c >= 'A' && c <= 'Z' ? (U8)(c + 32) : c; }

S64 CompareNCS(const void *a, const void *b, U64 n)
{
	const U8 *x = a, *y = b;
	U64 i;

	for (i = 0; i < n && lower(x[i]) == lower(y[i]); i++)
		;
	return i == n ? -1 : (S64)i;
}

/* ---- C library (GCC may also call these itself) ---- */

void *memcpy(void *d, const void *s, size_t n)
{
	U8 *pd = d;
	const U8 *ps = s;

	while (n--)
		*pd++ = *ps++;
	return d;
}

void *memset(void *d, int c, size_t n)
{
	U8 *p = d;

	while (n--)
		*p++ = (U8)c;
	return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const U8 *x = a, *y = b;

	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

size_t strlen(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

/* ---- file system (MFILES.H) ---- */

erc_t OpenFile(const char *n, U32 cb, U32 mode, U32 stream, U32 *ph)
					{ return CALL(OpenFile, A(n), cb, mode, stream, A(ph)); }
erc_t CloseFile(U32 h)			{ return CALL(CloseFile, h); }
erc_t CreateFile(const char *n, U32 cb, U32 attr)	{ return CALL(CreateFile, A(n), cb, attr); }
erc_t DeleteFile(U32 h)			{ return CALL(DeleteFile, h); }
erc_t RenameFile(const char *a, U32 ca, const char *b, U32 cb)
					{ return CALL(RenameFile, A(a), ca, A(b), cb); }
erc_t CreateDir(const char *n, U32 cb)	{ return CALL(CreateDir, A(n), cb, 0); }
erc_t DeleteDir(const char *n, U32 cb)	{ return CALL(DeleteDir, A(n), cb, 0); }
erc_t ReadBytes(U32 h, U8 *p, U64 n, U64 *pn)	{ return CALL(ReadBytes, h, A(p), n, A(pn)); }
erc_t WriteBytes(U32 h, const U8 *p, U64 n, U64 *pn)	{ return CALL(WriteBytes, h, A(p), n, A(pn)); }
erc_t ReadBlock(U32 h, U8 *p, U64 n, U64 lfa, U64 *pn)	{ return CALL(ReadBlock, h, A(p), n, lfa, A(pn)); }
erc_t WriteBlock(U32 h, const U8 *p, U64 n, U64 lfa, U64 *pn)
					{ return CALL(WriteBlock, h, A(p), n, lfa, A(pn)); }
erc_t GetFileSize(U32 h, U64 *p)	{ return CALL(GetFileSize, h, A(p)); }
erc_t SetFileSize(U32 h, U64 s)		{ return CALL(SetFileSize, h, s); }
erc_t GetFileLFA(U32 h, U64 *p)		{ return CALL(GetFileLFA, h, A(p)); }
erc_t SetFileLFA(U32 h, U64 lfa)	{ return CALL(SetFileLFA, h, lfa); }
erc_t GetDirSector(const char *p, U32 cb, U8 *s, U32 ss, U32 n)
					{ return CALL(GetDirSector, A(p), cb, A(s), ss, n); }
erc_t GetSystemDisk(U8 *p)		{ return CALL(GetSystemDisk, A(p)); }
erc_t ReadDir(const char *p, U32 cb, U32 i, struct dir_entry *e)
					{ return CALL(ReadDir, A(p), cb, i, A(e)); }
