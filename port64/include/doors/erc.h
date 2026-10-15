/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* MMURTL error/status codes (OSSOURCE/MOSEDF.INC, MSTATUS.H). Same numbers, so programs and the
   book stay valid. 0 = ErcOk (types.h). */
#ifndef DOORS_ERC_H
#define DOORS_ERC_H

/* 1-199: kernel and resource management */
#define ErcEOF          1	/* end of file */
#define ErcOpCancel     4	/* operator cancel (also: job killed with Ctrl-Alt-Del) */
#define ErcNullPtr      6	/* null pointer passed into call or service */
#define ErcOutOfRange   10	/* exchange number beyond allocated count */
#define ErcNotAlloc     11	/* bad exchange (not allocated) */
#define ErcNotOwner     12	/* exchange or request not owned by the caller */
#define ErcNoMsg        20	/* CheckMsg: no message waiting */
#define ErcNoSuchSvc    30	/* no service by that name */
#define ErcBadSvcCode   32	/* service doesn't handle that code */
#define ErcNotRqBlk     34	/* not a request block */
#define ErcOutOfSvcDesc 36	/* no more empty service descriptors */
#define ErcOwnerAbort   37	/* service code 0: the owner of this request is going away */
#define ErcNoMoreExch   40
#define ErcNoMoreLBs    41	/* out of link blocks */
#define ErcNoSvcBlks    43
#define ErcNoMoreTBs    44	/* out of timer blocks */
#define ErcNoMoreTSSs   45	/* out of tasks */
#define ErcNoMoreRqBlks 46	/* out of request blocks */
#define ErcNoMoreJCBs   47	/* out of job control blocks */
#define ErcBadCallGate  49	/* called an uninitialised call gate (here: unknown, not yet
				   ported, or kernel-only system call) */
#define ErcBadPriority  50
#define ErcBadJobNum    70
#define ErcInvalidJCB   71

/* 100-199: memory management */
#define ErcNoMem        101	/* not enough memory (no more pages) */
#define ErcBadMemPage   102	/* bad physical page */
#define ErcBadMemReq    104	/* invalid size for memory request */
#define ErcInternalMem  106
#define ErcBadLinAdd    108	/* bad linear address passed to DeAllocPage */
#define ErcShortMem     109	/* fewer valid pages than requested were deallocated */
#define ErcBadAlias     111	/* address passed in wasn't an alias */

/* 200-299: file system (MSTATUS.H) */
#define ErcBadFileSpec  200	/* invalid file spec (not correct format) */
#define ErcNoSuchDrive  201	/* no such drive letter */
#define ErcNotAFile     202	/* tried to open a directory */
#define ErcNoSuchFile   203
#define ErcNoSuchDir    204
#define ErcReadOnly     205	/* file or handle not open for modify */
#define ErcNoFreeFCB    206	/* too many open files */
#define ErcBadOpenMode  207
#define ErcFileInUse    208	/* open in an incompatible mode */
#define ErcNoFreeFUB    209
#define ErcBadFileHandle 210
#define ErcBrokenFile   211	/* cluster chain broken */
#define ErcBadFCB       213
#define ErcStreamFile   214	/* not allowed on a stream file */
#define ErcBlockFile    215	/* not allowed on a block file */
#define ErcBeyondEOF    217	/* SetFileLFA or ReadBlock/WriteBlock beyond EOF */
#define ErcNoParTable   218	/* no partition table */
#define ErcBadFATClstr  220	/* file system damaged */
#define ErcRenameDrv    222	/* rename across volumes */
#define ErcRenameDir    223
#define ErcNoMatch      224	/* no matching directory entry / sector */
#define ErcWriteOnly    225
#define ErcDupName      226	/* name exists as a file or directory already */
#define ErcNotSupported 227	/* not supported (DOORS also uses it for not-yet-ported features) */
#define ErcRootFull     228	/* FAT12/16 root directory full */
#define ErcDiskFull     230	/* no more free clusters */
#define ErcDirFull      231
#define ErcDirNotEmpty  232	/* DOORS: DeleteDir of a non-empty directory (MMURTL never finished it) */

/* 500-599: device management (MSTATUS.H) */
#define ErcBadDevNum    500	/* device number too large */
#define ErcDevInUse     501
#define ErcBadOpNum     503	/* operation the driver doesn't handle */
#define ErcNoDevice     504	/* driver installed but no device */
#define ErcNoDriver     505	/* no driver for that device number */
#define ErcDevTimeOut   508	/* DOORS: the device didn't answer in time */
#define ErcDevError     509	/* DOORS: the device reported an error */

#endif
