/* Request blocks and named system services (MMURTL KERNEL.ASM Request/Respond/MoveRequest,
   RQBCODE.ASM, SVCCODE.ASM). See include/doors/msg.h for the flow.

   Differences from MMURTL, on purpose:
   - Request stores dData2 from its own parameter. MMURTL's __Request loads dData2 from
     [EBP+20], which is dData0's slot (its own header documents dData2 at [EBP+12]), so services
     always received dData0 twice. Fixed here.
   - Respond and MoveRequest check that the handle is a request block in use (ErcNotRqBlk);
     MMURTL trusted it. */

#include <doors/console.h>
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

static struct RqBlk *rqbs, *free_rqb;
static U32 n_rqb_free;
static struct svc svcs[nSVC];

static U64 pool_bytes;

void rqb_init(void)
{
	U8 *p;
	U32 i, n;

	pool_bytes = PAGE_ROUND_UP(nRQB * sizeof(struct RqBlk));
	n = (U32)(pool_bytes / PAGE_SIZE);
	if (AllocOSPage(n, &p))
		panic("rqb: out of memory for request blocks");
	rqbs = (struct RqBlk *)p;
	for (i = 0; i < nRQB; i++) {
		rqbs[i].pNextRQB = free_rqb;
		free_rqb = &rqbs[i];
	}
	n_rqb_free = nRQB;

	/* Read-only window for user-mode services (they read their requests as MMURTL services
	   did). Made at boot, before any address space exists, so every space shares it. */
	for (i = 0; i < n; i++) {
		U64 pa;

		if (GetPhyAdd(1, p + i * PAGE_SIZE, &pa) ||
		    vmm_map(&kernel_space, RQB_UBASE + i * PAGE_SIZE, pa, PTE_U | PTE_NX))
			panic("rqb: cannot map the user window");
	}
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

	if (h < base || h >= base + nRQB * sizeof(struct RqBlk) ||
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

	for (i = 0; i < nRQB; i++) {
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
