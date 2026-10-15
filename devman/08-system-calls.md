<!-- DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. -->
# 8. System call reference

The numbers are MMURTL's call gate order: number = (gate selector − 40h) / 8, generated from
`OSSOURCE/MPUBLICS.ASM` into `port64/include/doors/syscalls.def` by `tools/gen_syscalls.sh`.
They never change. DOORS-only calls start at 110.

**Status:**

| Status | Meaning |
|---|---|
| **yes** | available to programs (stub in `user/lib/libdoors.c`, declared in `doors.h`) |
| **lib** | implemented in the user library; no kernel entry needed |
| **kernel** | exists in the kernel for drivers and services, but programs get `ErcBadCallGate` |
| **not yet** | not ported; returns `ErcBadCallGate` (49). Most come with the CLI and video work in M9 |

Signatures use DOORS types. Pointers, sizes, file positions and messages are 64-bit.

## Messaging and tasks

| # | Call | Status | Signature / purpose |
|---|---|---|---|
| 0 | WaitMsg | yes | `WaitMsg(U32 Exch, U64 *pMsgRet)`: block until a message (2 × U64) arrives |
| 1 | SendMsg | yes | `SendMsg(U32 Exch, U64 dMsg1, U64 dMsg2)` |
| 2 | ISendMsg | kernel | `SendMsg` for interrupt handlers |
| 3 | SetPriority | yes | `SetPriority(U32 bPriority)`: 0 (highest) to 31 |
| 4 | Request | yes | see [chapter 5](05-messaging.md#request-and-respond) |
| 5 | Respond | yes | `Respond(U64 dRqHndl, U32 dStatRet)` |
| 6 | CheckMsg | yes | `CheckMsg(U32 Exch, U64 *pMsgRet)`: `ErcNoMsg` if empty, never blocks |
| 7 | NewTask | kernel | create a task with explicit job, priority and stack |
| 8 | AllocExch | yes | `AllocExch(U32 *pExchRet)` |
| 9 | DeAllocExch | yes | `DeAllocExch(U32 Exch)` |
| 10 | Sleep | yes | `Sleep(U32 nTicks)`: 1 tick = 10 ms |
| 11 | Alarm | yes | `Alarm(U32 dExch, U32 nTicks)`: timer message to `dExch` |
| 14 | RegisterSvc | yes | `RegisterSvc(const char *pName, U32 dExch)`: 8-character name |
| 30 | KillAlarm | yes | `KillAlarm(U32 dExch)` |
| 32 | SpawnTask | yes | `SpawnTask(void (*pEntry)(void), U32 dPriority, U32 fDebug, U8 *pStack, U32 fOSCode)`: `pStack` = top of the stack, `fOSCode` must be 0 |
| 34 | GetTimerTick | yes | `GetTimerTick(U64 *pTickRet)`: ticks since boot |
| 49 | MoveRequest | yes | `MoveRequest(U64 dRqBlkHndl, U32 DestExch)`: pass a request on |
| 60 | GetTSSExch | yes | `GetTSSExch(U32 *pExchRet)`: the calling task's own exchange |
| 108 | UnRegisterSvc | yes | `UnRegisterSvc(const char *pName)` |

## Memory

| # | Call | Status | Signature / purpose |
|---|---|---|---|
| 12 | AllocOSPage | kernel | pages in the kernel region |
| 13 | AllocPage | yes | `AllocPage(U32 nPages, U8 **ppMemRet)` |
| 50 | DeAllocPage | yes | `DeAllocPage(U8 *pOrigMem, U32 nPages)` |
| 74 | QueryPages | yes | `QueryPages(U32 *pdPagesLeft)`: free pages in the system |
| 75 | GetPhyAdd | kernel | physical address of a page |
| 101 | DeAliasMem | yes | `DeAliasMem(U8 *pAliasMem, U64 dcbAliasBytes, U32 JobNum)` |
| 102 | AliasMem | yes | `AliasMem(U8 *pMem, U64 dcbMem, U32 dJobNum, U8 **ppAliasRet)` |
| 103 | GetDMACount | not yet | ISA DMA |
| 104 | AllocDMAPage | kernel | pages below 16 MB for ISA DMA |
| 15 | DMASetUp | not yet | ISA DMA |
| 42 | CopyData | lib | `CopyData(const void *pSource, void *pDestination, U64 dBytes)` |
| 43 | CopyDataR | lib | the same, copying from the end (overlapping buffers) |
| 44 | FillData | lib | `FillData(void *pDest, U64 cBytes, U8 bFill)` |
| 45 | CompareNCS | lib | `CompareNCS(const void *a, const void *b, U64 n)`: like `Compare`, ignoring the case of A–Z |
| 46 | Compare | lib | `Compare(const void *a, const void *b, U64 n)`: **-1** if the `n` bytes are equal (MMURTL), else the offset of the first difference. Returns `S64` |

## Jobs

| # | Call | Status | Signature / purpose |
|---|---|---|---|
| 51 | LoadNewJob | yes | `LoadNewJob(const char *pFileName, U32 cbFileName, U32 *pJobNumRet)`: boot module name or file path |
| 78 | GetJobNum | yes | `GetJobNum(U32 *pJobNumRet)` |
| 79 | ExitJob | yes | `ExitJob(erc_t dError)`: never returns. If an exit run file is set (`SetExitJob`), it is loaded into the same job; otherwise the job ends |
| 82 | SetCmdLine | yes | `SetCmdLine(const char *pCmd, U32 dcbCmd)`: up to 79 characters |
| 83 | GetCmdLine | yes | `GetCmdLine(char *pCmdRet, U32 *pdcbCmdRet)` (buffer of 80 bytes is enough) |
| 105 | SetJobName | yes | `SetJobName(const char *pName, U32 dcbName)` |
| 106 | KillJob | yes | `KillJob(U32 JobNum)`: ends another job |
| 73 | GetpJCB | not yet | pointer to a job control block |
| 80 / 81 | SetUserName / GetUserName | yes | `SetUserName(const char *pUser, U32 dcbUser)` (max 29) / `GetUserName(char *pUserRet, U32 *pdcbUserRet)` |
| 84 / 85 | SetPath / GetPath | yes | `SetPath(const char *pPath, U32 dcbPath)` (max 69) / `GetPath(U32 JobNum, char *pPathRet, U32 *pdcbPathRet)`: the job's default path; `GetPath` works for any job |
| 86 / 87 | SetExitJob / GetExitJob | yes | `SetExitJob(const char *pRunFile, U32 dcbRunFile)` (max 79, 0 clears) / `GetExitJob(char *pRunRet, U32 *pdcbRunRet)`: the program `ExitJob` loads into the job |
| 88–91 | SetSysIn / SetSysOut / GetSysIn / GetSysOut | yes | the names are kept (max 49) for programs that read them; DOORS itself does not redirect |
| 96 | Chain | yes | `Chain(const char *pFileName, U32 cbFileName, erc_t dExitError)`: run another program in this job; it keeps the job number, screen, command line, path, user name and exit run file. Returns only if the program can't be loaded |

## Files

All described in [chapter 6](06-file-system.md).

| # | Call | Status | Signature |
|---|---|---|---|
| 61 | OpenFile | yes | `OpenFile(const char *pName, U32 cbName, U32 Mode, U32 fStream, U32 *pdHandleRet)` |
| 62 | CloseFile | yes | `CloseFile(U32 dHandle)` |
| 63 | ReadBlock | yes | `ReadBlock(U32 dHandle, U8 *pBytesRet, U64 nBytes, U64 dLFA, U64 *pdnBytesRet)` |
| 64 | WriteBlock | yes | `WriteBlock(U32 dHandle, const U8 *pData, U64 nBytes, U64 dLFA, U64 *pdnBytesRet)` |
| 65 | ReadBytes | yes | `ReadBytes(U32 dHandle, U8 *pBytesRet, U64 nBytes, U64 *pdBytesRet)` |
| 66 | WriteBytes | yes | `WriteBytes(U32 dHandle, const U8 *pBytes, U64 nBytes, U64 *pdBytesRet)` |
| 67 | GetFileLFA | yes | `GetFileLFA(U32 dHandle, U64 *pdLFARet)` |
| 68 | SetFileLFA | yes | `SetFileLFA(U32 dHandle, U64 dLFA)` |
| 69 | GetFileSize | yes | `GetFileSize(U32 dHandle, U64 *pdSizeRet)` |
| 70 | CreateFile | yes | `CreateFile(const char *pName, U32 cbName, U32 Attrib)` |
| 71 | RenameFile | yes | `RenameFile(const char *pCrntName, U32 cbCrntName, const char *pNewName, U32 cbNewName)` |
| 72 | DeleteFile | yes | `DeleteFile(U32 dHandle)` |
| 77 | GetDirSector | yes | `GetDirSector(const char *pPathSpec, U32 cbPathSpec, U8 *pSectorRet, U32 sSectorRet, U32 SectorNum)`: FAT only |
| 97 | SetFileSize | yes | `SetFileSize(U32 dHandle, U64 dSize)` |
| 99 | CreateDir | yes | `CreateDir(const char *pName, U32 cbName)` |
| 100 | DeleteDir | yes | `DeleteDir(const char *pName, U32 cbName)` |
| 107 | GetSystemDisk | yes | `GetSystemDisk(U8 *pDiskRet)` |
| 109 | undelete | not yet | (MMURTL placeholder) |
| **110** | **ReadDir** | yes | DOORS: `ReadDir(const char *pPath, U32 cbPath, U32 dIndex, struct dir_entry *pEntryRet)` |

## Screen and keyboard

Every job has its own 80×25 virtual screen. The calls act on the caller's job's screen. The
*video owner* is the job whose screen is shown, and the keyboard goes to it. Columns are 0–79 and
lines 0–24. Coordinates out of range give `ErcVidParam` (301), and a bad job number gives
`ErcVidNum` (300). Attributes are MMURTL's colour bytes (`doors.h`: `WHITE`, `BGBLUE`, ...).

| # | Call | Status | Signature / purpose |
|---|---|---|---|
| 55 | TTYOut | yes | `TTYOut(const char *pTextOut, U64 ddTextOut, U32 ddAttrib)`: text at the cursor. LF = next line at column 0, CR = column 0, BS = one left (not erasing), TAB = next multiple of 8 (DOORS). The screen scrolls at the bottom. Text is also copied to COM1 |
| 56 | PutVidChars | yes | `PutVidChars(U32 ddCol, U32 ddLine, const char *pChars, U32 sChars, U32 ddAttrib)`: characters at a position; the cursor doesn't move |
| 92 | PutVidAttrs | yes | `PutVidAttrs(U32 ddCol, U32 ddLine, U32 sChars, U32 dAttr)`: change colours only |
| 93 | GetVidChar | yes | `GetVidChar(U32 ddCol, U32 ddLine, U8 *pCharRet, U8 *pAttrRet)` |
| 57 / 58 | SetXY / GetXY | yes | `SetXY(U32 NewX, U32 NewY)` / `GetXY(U32 *pXRet, U32 *pYRet)`: cursor position |
| 76 | ScrollVid | yes | `ScrollVid(U32 ddULCol, U32 ddULline, U32 nddCols, U32 nddLines, U32 ddfUp)`: scroll a rectangle one line up (`ddfUp` ≠ 0) or down |
| 54 | ClrScr | yes | `ClrScr(void)`: fill with the normal attribute, cursor to 0,0 |
| 94 / 95 | SetNormVid / GetNormVid | yes | `SetNormVid(U32 dAttr)` / `GetNormVid(U32 *pdNormVidRet)`: the attribute `ClrScr` and `ScrollVid` fill with (default 07h) |
| 52 / 53 | SetVidOwner / GetVidOwner | yes | `SetVidOwner(U32 dJobNum)` / `GetVidOwner(U32 *pdVidNumRet)`: which job is on screen (job 2 is refused) |
| 59 | EditLine | lib | `EditLine(U8 *pStr, U32 dCrntLen, U32 dMaxLen, U32 *pdLenRet, U8 *pbExitChar, U32 dEditAttr)`: edit a line (at most 80) at the cursor. Backspace/Left delete, other keys end the edit and are returned in `*pbExitChar`. `ErcEditParam` (300) for bad lengths |
| 16 | ReadKbd | yes | `ReadKbd(U32 *pKeyCodeRet, U32 fWait)`: the next key for the caller's job. Waits if `fWait`, else `ErcNoKeyAvail` (700). See *Key codes* below |
| 28 / 29 | Beep / Tone | yes | `Beep(void)`: 800 Hz for 350 ms. `Tone(U32 dHz, U32 dTicks10ms)`: PC speaker (silent in QEMU unless an audio device is attached; the call still takes its time) |

### Key codes

`ReadKbd` returns MMURTL's key codes, translated with the tables of `KEYBOARD.ASM`:

| Bits | Contents |
|---|---|
| 0–7 | the key: ASCII 20h–7Eh, or a special code (below) |
| 8–15 | shift state: Ctrl `0x0300`, Shift `0x0C00`, Alt `0x3000` (`KBD_CTRL`, `KBD_SHIFT`, `KBD_ALT`) |
| 16–23 | lock state: Scroll 01h, Num 02h, Caps 04h |
| 24 | the key came from the numeric pad (`KBD_NUMPAD`) |

Special keys: Up 01h, Down 02h, Left 03h, Right 04h, PgUp 05h, Home 06h, Backspace 08h, Tab 09h,
End 0Bh, PgDn 0Ch, Enter 0Dh, Insert 0Eh, F1–F10 0Fh–18h, F11 19h, F12 1Ah, Esc 1Bh, Delete 7Fh.

Ctrl+Alt+key combinations are **global**: no program receives them. Ctrl-Alt-PgDn shows the next
job's screen, and Ctrl-Alt-Del kills the job on screen (never job 1).

## Devices, interrupts and ports

Drivers live in the kernel, so programs can't use these calls.

| # | Call | Status |
|---|---|---|
| 24 | InitDevDr | not yet |
| 25 / 26 / 27 | DeviceInit / DeviceOp / DeviceStat | kernel (the disk driver uses them) |
| 17 / 18 | AddCallGate / AddIDTGate | not yet (no call gates in DOORS) |
| 19–23 | EndOfIRQ / MaskIRQ / UnMaskIRQ / SetIRQVector / GetIRQVector | not yet |
| 31 | MicroDelay | not yet |
| 33 / 98 | GetCMOSTime / GetCMOSDate | yes. `GetCMOSTime(U32 *)`: BCD seconds in bits 0–7, minutes 8–15, hours 16–23. `GetCMOSDate(U32 *)`: BCD day of week (0 = Sunday) in bits 0–7, day 8–15, month 16–23, year (0–99) 24–31 |
| 41 | ReadCMOS | not yet |
| 35–40, 47, 48 | OutByte / OutWord / OutDWord / InByte / InWord / InDWord / InWords / OutWords | not yet |

## Adding a system call

1. Add `X(nnn, Name)` at the end of the DOORS block in `tools/gen_syscalls.sh` and run it. Never
   renumber an existing call.
2. Add `case SYS_Name:` in `kernel/syscall.c`. Check every user pointer with `urange`, `copy_in` or
   `copy_out`.
3. Add a stub in `user/lib/libdoors.c` and the prototype in `user/lib/doors.h`.
4. Add a test (kernel self-test or a user program) and update this chapter.
