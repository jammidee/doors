<!-- DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. -->
# 9. Error codes

Every call returns an `erc_t` (32-bit). `0` (`ErcOk`) means success. The codes are MMURTL's
(`MOSEDF.INC`), with the same numbers, in `port64/include/doors/erc.h`. Codes marked
**DOORS** are new.

## General and messaging

| Code | Name | Meaning |
|---|---|---|
| 0 | ErcOk | success |
| 1 | ErcEOF | end of file: fewer bytes were read than asked for |
| 4 | ErcOpCancel | operation cancelled |
| 6 | ErcNullPtr | a null pointer was passed |
| 10 | ErcOutOfRange | exchange number out of range |
| 11 | ErcNotAlloc | exchange not allocated (also: the exchange was freed while a task waited on it) |
| 12 | ErcNotOwner | exchange or request belongs to another job |
| 20 | ErcNoMsg | `CheckMsg`: no message waiting |
| 30 | ErcNoSuchSvc | no service registered under that name |
| 32 | ErcBadSvcCode | the service doesn't handle that service code |
| 34 | ErcNotRqBlk | not a valid request handle |
| 36 | ErcOutOfSvcDesc | all 64 service names in use |
| 37 | ErcOwnerAbort | service code 0: the requester is going away |
| 40 | ErcNoMoreExch | all exchanges in use |
| 41 | ErcNoMoreLBs | all link blocks (queued messages) in use |
| 43 | ErcNoSvcBlks | no service blocks |
| 44 | ErcNoMoreTBs | all timer blocks in use (too many `Sleep`/`Alarm` pending) |
| 45 | ErcNoMoreTSSs | all tasks in use |
| 46 | ErcNoMoreRqBlks | all request blocks in use |
| 47 | ErcNoMoreJCBs | all jobs in use |
| 49 | ErcBadCallGate | the call is not available: not ported yet, or kernel only |
| 50 | ErcBadPriority | priority outside 0–31 |

## Jobs

| Code | Name | Meaning |
|---|---|---|
| 70 | ErcBadJobNum | no such job (or a system job) |
| 71 | ErcInvalidJCB | invalid job control block |
| 74 | ErcBadRunFile | not a valid program (DOORS: not a loadable static ELF64) |
| 76 | ErcBadJobParam | a string too long for its job field (`SetPath` 69, `SetUserName` 29, `SetExitJob` 79, `SetSysIn`/`SetSysOut` 49) |
| 1000 + n | ErcCpuFault | **DOORS:** the job was ended by CPU exception `n`. For example: 1000 divide error, 1006 invalid opcode, 1013 general protection, 1014 page fault |

## Memory

| Code | Name | Meaning |
|---|---|---|
| 101 | ErcNoMem | not enough free memory |
| 102 | ErcBadMemPage | bad physical page |
| 104 | ErcBadMemReq | invalid size (for example 0 pages, or a buffer too small for the call) |
| 106 | ErcInternalMem | internal memory error |
| 108 | ErcBadLinAdd | bad address: not mapped, not in the user half, or not writable where it must be |
| 109 | ErcShortMem | fewer valid pages than requested were freed |
| 111 | ErcBadAlias | the address is not an alias |

## Files

| Code | Name | Meaning |
|---|---|---|
| 200 | ErcBadFileSpec | malformed path (no drive letter, too long), or a block call with a size or position that is not a multiple of 512 |
| 201 | ErcNoSuchDrive | no volume with that letter |
| 202 | ErcNotAFile | the name is a directory |
| 203 | ErcNoSuchFile | file not found |
| 204 | ErcNoSuchDir | directory not found |
| 205 | ErcReadOnly | the handle is not open with `ModeModify`, the file is read-only, or the volume is mounted read-only |
| 206 | ErcNoFreeFCB | too many open files (128) |
| 207 | ErcBadOpenMode | mode other than `ModeRead`/`ModeModify` |
| 208 | ErcFileInUse | open in a conflicting mode (or open while being renamed) |
| 209 | ErcNoFreeFUB | no free file user block |
| 210 | ErcBadFileHandle | not a handle of this job |
| 211 | ErcBrokenFile | the file's data structures are broken: a FAT cluster chain, or on ext4 a bad inode, extent or checksum (the console also prints `checksum mismatch`) |
| 213 | ErcBadFCB | bad file control block |
| 214 | ErcStreamFile | block call on a stream file |
| 215 | ErcBlockFile | stream call on a block file |
| 217 | ErcBeyondEOF | position past the end of the file |
| 218 | ErcNoParTable | no partition table |
| 220 | ErcBadFATClstr | the FAT file system is damaged (bad cluster number) |
| 222 | ErcRenameDrv | rename across volumes |
| 223 | ErcRenameDir | invalid directory rename (MMURTL; not returned by DOORS at present) |
| 224 | ErcNoMatch | no such directory entry or sector (`ReadDir`/`GetDirSector` past the end) |
| 225 | ErcWriteOnly | write-only |
| 226 | ErcDupName | the name already exists |
| 227 | ErcNotSupported | not supported on this file system or volume (see [chapter 6](06-file-system.md#ext4-limits-until-milestone-m8b)) |
| 228 | ErcRootFull | FAT12/16 root directory full |
| 230 | ErcDiskFull | no free space on the volume |
| 231 | ErcDirFull | directory full |
| 232 | ErcDirNotEmpty | **DOORS:** `DeleteDir` of a directory that still has entries |

## Video and keyboard

| Code | Name | Meaning |
|---|---|---|
| 300 | ErcVidNum | bad video (job) number for `SetVidOwner` |
| 300 | ErcEditParam | bad length passed to `EditLine` (MMURTL uses the same number) |
| 301 | ErcVidParam | column, line or count outside the 80×25 screen |
| 700 | ErcNoKeyAvail | `ReadKbd` without waiting: no key in the buffer |

## Devices

| Code | Name | Meaning |
|---|---|---|
| 500 | ErcBadDevNum | device number too large |
| 501 | ErcDevInUse | device in use |
| 503 | ErcBadOpNum | operation not supported by the driver |
| 504 | ErcNoDevice | driver present, no device |
| 505 | ErcNoDriver | no driver for that device |
| 508 | ErcDevTimeOut | **DOORS:** the device did not answer in time |
| 509 | ErcDevError | **DOORS:** the device reported an error |
