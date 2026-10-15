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

/* 200-299: file system (MSTATUS.H) — used from M7 on */
#define ErcNotSupported 227	/* not supported (DOORS also uses it for not-yet-ported features) */

#endif
