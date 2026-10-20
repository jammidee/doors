/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Request blocks and named system services (MMURTL KERNEL.ASM Request/Respond/MoveRequest,
   RQBCODE.ASM, SVCCODE.ASM). See include/doors/msg.h for the flow.

   Differences from MMURTL, on purpose:
   - Request stores dData2 from its own parameter. MMURTL's __Request loads dData2 from
     [EBP+20], which is dData0's slot (its own header documents dData2 at [EBP+12]), so services
     always received dData0 twice. Fixed here.
   - Respond and MoveRequest check that the handle is a request block in use (ErcNotRqBlk);
     MMURTL trusted it. */

#include <doors/console.h>
#include <doors/gtable.h>
#include <doors/mem.h>
#include <doors/msg.h>
#include <doors/string.h>
#include <doors/task.h>
#include <doors/x86.h>

struct svc {
	char name[8];
	U32 exch;
	bool used;
};

/* Request blocks: a growable table (ROADMAP 3.1), also mapped read-only for user-mode services at
   RQB_UBASE (they read their requests as MMURTL services did). Every page the table gets is mapped
   in both places; the kernel half's page tables are shared, so all address spaces see it.
   nRQB is the ceiling; n_rqb_free counts against it. */
static struct gtable gt_rqb;
static struct RqBlk *rqbs, *free_rqb;
static U32 n_rqb_free;
static struct svc svcs[nSVC];

#define pool_bytes (gt_rqb.pages * PAGE_SIZE)

static bool rqb_more(void)
{
	U32 i, old = gt_rqb.n;

	if (!gt_grow(&gt_rqb, gt_rqb.n + 64))
		return false;
	for (i = gt_rqb.n; i-- > old;) {
		rqbs[i].pNextRQB = free_rqb;
		free_rqb = &rqbs[i];
	}
	return true;
}

void rqb_init(void)
{
	gt_init(&gt_rqb, "request blocks", GT_RQB, sizeof(struct RqBlk), nRQB, 0, RQB_UBASE);
	rqbs = (struct RqBlk *)(void *)gt_rqb.base;
	while (gt_rqb.n < 256)
		if (!rqb_more())
			panic("rqb: out of memory for request blocks");
	n_rqb_free = nRQB;
}

/* Kernel handle <-> user handle (the same request block seen through RQB_UBASE). */
U64 rqb_to_user(U64 h)
{
	U64 base = (U64)(uintptr_t)rqbs;

	return h >= base && h < base + pool_bytes ? RQB_UBASE + (h - base) : h;
}

U64 rqb_from_user(U64 h)
{
	return h >= RQB_UBASE && h < RQB_UBASE + pool_bytes ?
		(U64)(uintptr_t)rqbs + (h - RQB_UBASE) : h;
}

U32 rqb_free_count(void)
{
	return n_rqb_free;
}

U32 svc_used(U32 i)
{
	return i < nSVC && svcs[i].used;
}

static struct RqBlk *rqb_from(U64 h)
{
	U64 base = (U64)(uintptr_t)rqbs;

	if (h < base || h >= base + (U64)gt_rqb.n * sizeof(struct RqBlk) ||
	    (h - base) % sizeof(struct RqBlk))
		return NULL;
	return ((struct RqBlk *)(uintptr_t)h)->inuse ? (struct RqBlk *)(uintptr_t)h : NULL;
}

/* Interrupts off. */
void rqb_dispose(U64 h)
{
	struct RqBlk *r = rqb_from(h);

	if (!r)
		return;
	memset(r, 0, sizeof(*r));
	r->pNextRQB = free_rqb;
	free_rqb = r;
	n_rqb_free++;
}

static bool writable(U32 job, U8 *p, U64 cb)
{
	struct addr_space *as = job_space(job);
	U64 va = (U64)(uintptr_t)p & PAGE_MASK, end = (U64)(uintptr_t)p + cb, pte;

	if (!p || !cb || (U64)(uintptr_t)p >= 0xFFFF800000000000UL)
		return true;		/* nothing to check / kernel memory (kernel requesters only) */
	for (; as && va < end; va += PAGE_SIZE)
		if (!vmm_translate(as, va, NULL, &pte) || !(pte & PTE_W))
			return false;
	return as != NULL;
}

/* Service side (task context, interrupts on), from WaitMsg/CheckMsg: map the requester's buffers
   into this job and put the alias addresses into the RqBlk, as MMURTL's WaitMsg did. */
erc_t rqb_received(U64 h)
{
	struct RqBlk *r = rqb_from(h);
	erc_t erc;

	if (!r)
		return ErcNotRqBlk;
	if (r->orphan || job_space(r->RqOwnerJob) == current_space)
		return ErcOk;		/* requester gone, or same address space: nothing to alias */
	/* Receive buffers (after the first npSend) are written by the service: every page must be
	   writable in the requester's space, or a service in ring 0 would fault writing it. */
	if ((r->npSend < 1 && !writable(r->RqOwnerJob, r->pOrig1, r->cbData1)) ||
	    (r->npSend < 2 && !writable(r->RqOwnerJob, r->pOrig2, r->cbData2)))
		return ErcBadLinAdd;
	if (r->cbData1 && r->pData1) {
		erc = AliasMem(r->pOrig1, r->cbData1, r->RqOwnerJob, &r->pData1);
		if (erc)
			return erc;
	}
	if (r->cbData2 && r->pData2) {
		erc = AliasMem(r->pOrig2, r->cbData2, r->RqOwnerJob, &r->pData2);
		if (erc) {
			if (r->cbData1 && r->pData1)
				DeAliasMem(r->pData1, r->cbData1, r->RqOwnerJob);
			r->pData1 = r->pOrig1;
			return erc;
		}
	}
	r->svc_job = current->job;
	return ErcOk;
}

static struct svc *find_svc(const char *name)
{
	U32 i;

	for (i = 0; i < nSVC; i++)
		if (svcs[i].used && !memcmp(svcs[i].name, name, 8))
			return &svcs[i];
	return NULL;
}

erc_t RegisterSvc(const char *pName, U32 dExch)
{
	U64 f;
	struct svc *s;
	erc_t erc;
	U32 i;

	if (!pName)
		return ErcNullPtr;
	erc = exch_check(dExch, NULL);
	if (erc)
		return erc;
	f = irq_save();
	s = find_svc(pName);
	for (i = 0; !s && i < nSVC; i++)
		if (!svcs[i].used)
			s = &svcs[i];
	if (s) {
		memcpy(s->name, pName, 8);
		s->exch = dExch;		/* re-registering a name moves it to the new exchange */
		s->used = true;
	} else {
		erc = ErcOutOfSvcDesc;
	}
	irq_restore(f);
	return erc;
}

erc_t UnRegisterSvc(const char *pName)
{
	U64 f;
	struct svc *s;

	if (!pName)
		return ErcNullPtr;
	f = irq_save();
	s = find_svc(pName);
	if (s)
		memset(s, 0, sizeof(*s));
	irq_restore(f);
	return s ? ErcOk : ErcNoSuchSvc;
}

erc_t Request(const char *pSvcName, U32 wSvcCode, U32 dRespExch, U64 *pRqHndlRet,
	      U32 dnpSend, U8 *pData1, U64 dcbData1, U8 *pData2, U64 dcbData2,
	      U32 dData0, U32 dData1, U32 dData2)
{
	struct svc *s;
	struct RqBlk *r;
	erc_t erc;
	U32 svc_exch;
	U64 f;

	if (!pSvcName || !pRqHndlRet)
		return ErcNullPtr;
	erc = exch_check(dRespExch, NULL);
	if (erc)
		return erc;

	f = irq_save();
	s = find_svc(pSvcName);
	if (!s) {
		irq_restore(f);
		return ErcNoSuchSvc;
	}
	svc_exch = s->exch;
	if (!free_rqb)
		rqb_more();			/* the table grows; at its ceiling or without memory: */
	r = free_rqb;
	if (!r) {
		irq_restore(f);
		return ErcNoMoreRqBlks;
	}
	free_rqb = r->pNextRQB;
	n_rqb_free--;
	memset(r, 0, sizeof(*r));
	r->inuse = 1;
	r->ServiceExch = svc_exch;
	r->RespExch = dRespExch;
	r->RqOwnerJob = current->job;
	r->ServiceCode = (U16)wSvcCode;
	r->dData0 = dData0;
	r->dData1 = dData1;
	r->dData2 = dData2;			/* MMURTL bug fixed: it stored dData0 here */
	r->npSend = (U8)(dnpSend > 2 ? 2 : dnpSend);
	r->npRecv = (U8)(2 - r->npSend);
	r->pData1 = r->pOrig1 = pData1;
	r->cbData1 = dcbData1;
	r->pData2 = r->pOrig2 = pData2;
	r->cbData2 = dcbData2;
	*pRqHndlRet = (U64)(uintptr_t)r;

	erc = msg_post(svc_exch, (U64)(uintptr_t)r, 0, LB_REQ, true);
	if (erc)
		rqb_dispose((U64)(uintptr_t)r);
	irq_restore(f);
	return erc;
}

erc_t Respond(U64 dRqHndl, U32 dStatRet)
{
	struct RqBlk *r;
	erc_t erc;
	U32 resp;
	U64 f = irq_save();

	r = rqb_from(dRqHndl);
	if (!r) {
		irq_restore(f);
		return ErcNotRqBlk;
	}
	if (dStatRet == ErcOwnerAbort || r->orphan) {	/* MMURTL: just free the RqBlk */
		rqb_dispose(dRqHndl);
		irq_restore(f);
		return ErcOk;
	}
	resp = r->RespExch;
	erc = exch_check(resp, NULL);
	if (erc) {
		irq_restore(f);
		return erc;
	}
	if (r->svc_job) {			/* remove the aliases made in rqb_received */
		if (r->cbData1 && r->pData1)
			DeAliasMem(r->pData1, r->cbData1, r->RqOwnerJob);
		if (r->cbData2 && r->pData2)
			DeAliasMem(r->pData2, r->cbData2, r->RqOwnerJob);
	}
	rqb_dispose(dRqHndl);
	erc = msg_post(resp, dRqHndl, dStatRet, LB_RESP, true);
	irq_restore(f);
	return erc;
}

erc_t MoveRequest(U64 dRqBlkHndl, U32 DestExch)
{
	U32 owner;
	erc_t erc;
	U64 f = irq_save();

	if (!rqb_from(dRqBlkHndl)) {
		irq_restore(f);
		return ErcNotRqBlk;
	}
	erc = exch_check(DestExch, &owner);
	if (!erc && owner != current->job)
		erc = ErcNotOwner;	/* only to an exchange of the same job (service) */
	if (!erc)
		erc = msg_post(DestExch, dRqBlkHndl, 0, LB_REQA, true);	/* already aliased */
	irq_restore(f);
	return erc;
}

/* ---- jobs going away (kernel/job.c), interrupts off ---- */

/* A request that can't be served: tell the requester (h, erc) and free the block. */
void rqb_fail(U64 h, U32 erc)
{
	struct RqBlk *r = rqb_from(h);

	if (!r)
		return;
	if (!r->orphan)
		msg_post(r->RespExch, h, erc, LB_RESP, false);
	rqb_dispose(h);
}

/* Unmap a service's alias of a requester's buffer, in the service's address space. */
static void unmap_alias(struct addr_space *as, U8 *p, U64 cb)
{
	U64 va = (U64)(uintptr_t)p & PAGE_MASK, end = (U64)(uintptr_t)p + cb, pte;

	for (; as && va < end; va += PAGE_SIZE)
		if (vmm_translate(as, va, NULL, &pte) && (pte & PTE_ALIAS))
			vmm_unmap(as, va);
}

/* Job `job` is ending:
   - requests it made become orphans: the service's aliases of its (soon freed) pages are
     removed now, the buffers are dropped, and the eventual Respond is discarded;
   - requests it was serving (aliased into its space) are failed back to their requesters. */
void rqb_job_gone(U32 job)
{
	U32 i;

	for (i = 0; i < gt_rqb.n; i++) {
		struct RqBlk *r = &rqbs[i];

		if (!r->inuse)
			continue;
		if (r->RqOwnerJob == job && !r->orphan) {
			if (r->svc_job) {
				unmap_alias(job_space(r->svc_job), r->pData1, r->cbData1);
				unmap_alias(job_space(r->svc_job), r->pData2, r->cbData2);
			}
			r->orphan = 1;
			r->svc_job = 0;
			r->pData1 = r->pData2 = r->pOrig1 = r->pOrig2 = NULL;
			r->cbData1 = r->cbData2 = 0;
		} else if (r->svc_job == job) {
			rqb_fail((U64)(uintptr_t)r, ErcNoSuchSvc);
		}
	}
}

/* Services registered on exchanges owned by `job` disappear with it. */
void svc_job_gone(U32 job)
{
	U32 i, owner;

	for (i = 0; i < nSVC; i++)
		if (svcs[i].used && exch_check(svcs[i].exch, &owner) == ErcOk && owner == job)
			memset(&svcs[i], 0, sizeof(svcs[i]));
}

bool svc_registered(const char *name)
{
	U64 f = irq_save();
	bool r = find_svc(name) != NULL;

	irq_restore(f);
	return r;
}
