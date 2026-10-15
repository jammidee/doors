/* The FILESYS service and the MMURTL file calls (FSYS.C, MFILES.H).

   As in MMURTL, one service task does all file system work, one request at a time (it runs at
   priority 5, like MMURTL's FSysTask). OpenFile, ReadBytes, ... build a Request to "FILESYS ",
   wait for the response on the caller's own exchange and return its status. A request carries:
     pData1 / cbData1  the path, or the caller's data buffer (aliased into the service's job)
     pData2 / cbData2  a struct fs_req with the other parameters and the results
   Handles belong to the job that opened them; a job's files are closed when it ends.

   Modes (MMURTL): ModeRead may be shared; ModeModify is exclusive. fStream != 0 opens a stream
   file (ReadBytes/WriteBytes at the current LFA); 0 opens a block file (ReadBlock/WriteBlock with
   512-byte multiples at an explicit LFA). WriteBlock extends the file, so a copy no longer loses
   the last partial block as MMURTL's did. */

#include <doors/console.h>
#include <doors/fs.h>
#include <doors/msg.h>
#include <doors/string.h>
#include <doors/task.h>
#include <doors/x86.h>

#define nFILES 128

enum { FS_OPEN = 1, FS_CLOSE, FS_CREATE, FS_DELETE, FS_RENAME, FS_MKDIR, FS_RMDIR, FS_READB,
       FS_WRITEB, FS_READBLK, FS_WRITEBLK, FS_GETSIZE, FS_SETSIZE, FS_GETLFA, FS_SETLFA,
       FS_DIRSEC };

struct fs_req {
	U32 handle, mode, stream, attr, sectnum;
	U64 n, lfa, out;
	U32 out32;
	U32 name2_len;
	char name2[MAX_PATH];		/* RenameFile's new name / GetDirSector's path */
};

struct ofile {
	bool used;
	U32 job;
	struct fat_node n;
	U64 lfa;
	U8 mode, stream;
};

static struct ofile files[nFILES];
static U32 fs_exch;

static bool same(const struct fat_node *a, const struct fat_node *b)
{
	return a->vol == b->vol && a->dir_clus == b->dir_clus && a->idx == b->idx;
}

static struct ofile *handle(U32 h, U32 job)
{
	return h < nFILES && files[h].used && files[h].job == job ? &files[h] : NULL;
}

static bool is_open(const struct fat_node *n, int want_modify, bool *modifier)
{
	U32 i;
	bool any = false;

	*modifier = false;
	for (i = 0; i < nFILES; i++)
		if (files[i].used && same(&files[i].n, n)) {
			any = true;
			if (files[i].mode == ModeModify)
				*modifier = true;
		}
	(void)want_modify;
	return any;
}

static erc_t do_open(struct RqBlk *r, struct fs_req *q)
{
	struct fat_node n;
	bool modifier;
	erc_t erc;
	U32 i;

	if (q->mode > ModeModify)
		return ErcBadOpenMode;
	if ((erc = fat_lookup((const char *)r->pData1, (U32)r->cbData1, &n)))
		return erc;
	if (n.attr & ATTR_DIR)
		return ErcNotAFile;
	if (q->mode == ModeModify && (n.attr & ATTR_RO))
		return ErcReadOnly;
	if (is_open(&n, q->mode, &modifier) && (q->mode == ModeModify || modifier))
		return ErcFileInUse;
	for (i = 0; i < nFILES; i++)
		if (!files[i].used)
			break;
	if (i == nFILES)
		return ErcNoFreeFCB;
	files[i].used = true;
	files[i].job = r->RqOwnerJob;
	files[i].n = n;
	files[i].lfa = 0;
	files[i].mode = (U8)q->mode;
	files[i].stream = q->stream ? 1 : 0;
	q->out32 = i;
	return ErcOk;
}

static erc_t serve(struct RqBlk *r, struct fs_req *q)
{
	struct ofile *o = NULL;
	struct fat_node n;
	bool modifier;
	erc_t erc;
	U64 len = r->cbData1 < q->n ? r->cbData1 : q->n;

	switch (r->ServiceCode) {
	case FS_OPEN:
		return do_open(r, q);
	case FS_CREATE:
		return fat_create((const char *)r->pData1, (U32)r->cbData1, (U8)(q->attr & ~ATTR_DIR), &n);
	case FS_MKDIR:
		return fat_create((const char *)r->pData1, (U32)r->cbData1, ATTR_DIR, &n);
	case FS_RENAME:
		if ((erc = fat_lookup((const char *)r->pData1, (U32)r->cbData1, &n)))
			return erc;
		if (is_open(&n, 0, &modifier))
			return ErcFileInUse;
		return fat_rename(&n, q->name2, q->name2_len);
	case FS_RMDIR:
		if ((erc = fat_lookup((const char *)r->pData1, (U32)r->cbData1, &n)))
			return erc == ErcNoSuchFile ? ErcNoSuchDir : erc;
		if (!(n.attr & ATTR_DIR) || n.is_root)
			return ErcNoSuchDir;
		if (!fat_dir_empty(&n))
			return ErcDirNotEmpty;
		return fat_remove(&n);
	case FS_DIRSEC:
		if ((erc = fat_lookup(q->name2, q->name2_len, &n)))
			return erc == ErcNoSuchFile ? ErcNoSuchDir : erc;
		if (r->cbData1 < 512)
			return ErcBadMemReq;
		return fat_dir_sector(&n, q->sectnum, r->pData1);
	}

	o = handle(q->handle, r->RqOwnerJob);
	if (!o)
		return ErcBadFileHandle;
	switch (r->ServiceCode) {
	case FS_CLOSE:
		o->used = false;
		return ErcOk;
	case FS_DELETE:
		if (o->mode != ModeModify)
			return ErcReadOnly;
		erc = fat_remove(&o->n);
		o->used = false;
		return erc;
	case FS_READB:
		if (!o->stream)
			return ErcBlockFile;
		erc = fat_read(&o->n, o->lfa, r->pData1, len, &q->out);
		o->lfa += q->out;
		return erc ? erc : q->out < q->n ? ErcEOF : ErcOk;
	case FS_WRITEB:
		if (!o->stream)
			return ErcBlockFile;
		if (o->mode != ModeModify)
			return ErcReadOnly;
		erc = fat_write(&o->n, o->lfa, r->pData1, len, &q->out);
		o->lfa += q->out;
		return erc;
	case FS_READBLK:
		if (o->stream)
			return ErcStreamFile;
		if (q->lfa % 512 || q->n % 512)
			return ErcBadFileSpec;
		if (q->lfa >= o->n.size)
			return ErcBeyondEOF;
		return fat_read(&o->n, q->lfa, r->pData1, len, &q->out);
	case FS_WRITEBLK:
		if (o->stream)
			return ErcStreamFile;
		if (o->mode != ModeModify)
			return ErcReadOnly;
		if (q->lfa % 512 || q->n % 512)
			return ErcBadFileSpec;
		return fat_write(&o->n, q->lfa, r->pData1, len, &q->out);
	case FS_GETSIZE:
		q->out = o->n.size;
		return ErcOk;
	case FS_SETSIZE:
		if (o->mode != ModeModify)
			return ErcReadOnly;
		erc = fat_truncate(&o->n, q->n);
		if (o->lfa > o->n.size)
			o->lfa = o->n.size;
		return erc;
	case FS_GETLFA:
		q->out = o->lfa;
		return ErcOk;
	case FS_SETLFA:
		if (q->lfa == ~0UL)
			q->lfa = o->n.size;
		if (q->lfa > o->n.size)
			return ErcBeyondEOF;
		o->lfa = q->lfa;
		return ErcOk;
	}
	return ErcBadSvcCode;
}

static void fsys_task(void *unused)
{
	U64 m[2];

	(void)unused;
	for (;;) {
		struct RqBlk *r;
		erc_t erc;

		erc = WaitMsg(fs_exch, m);
		if (erc && erc != ErcNoMem && erc != ErcBadLinAdd && erc != ErcBadJobNum)
			continue;
		r = (struct RqBlk *)(uintptr_t)m[0];
		if (erc) {				/* couldn't alias the caller's buffers */
			Respond(m[0], erc);
			continue;
		}
		if (r->orphan || r->ServiceCode == 0) {	/* requester gone / abort */
			Respond(m[0], ErcOk);
			continue;
		}
		if (!r->pData2 || r->cbData2 < sizeof(struct fs_req)) {
			Respond(m[0], ErcBadSvcCode);
			continue;
		}
		Respond(m[0], serve(r, (struct fs_req *)r->pData2));
	}
}

void fsys_init(void)
{
	erc_t erc;

	if (exch_alloc_job(1, &fs_exch) || RegisterSvc("FILESYS ", fs_exch) ||
	    !task_create("filesys", fsys_task, NULL, 5, 1, &erc))
		panic("FILESYS service did not start");
}

/* A job ended: its open files are simply closed (writes are already on disk). */
void fsys_job_gone(U32 job)
{
	U32 i;

	for (i = 0; i < nFILES; i++)
		if (files[i].used && files[i].job == job)
			files[i].used = false;
}

U32 fsys_open_count(void)
{
	U32 i, n = 0;

	for (i = 0; i < nFILES; i++)
		n += files[i].used;
	return n;
}

/* ---- the MMURTL calls: a request to the service, then wait for its answer ---- */

/* nsend: 1 if pData1 is only read by the service (path, data to write), 0 if the service writes
   it (ReadBytes, ReadBlock, GetDirSector). pData2 (struct fs_req) is always written. */
static erc_t call(U32 code, const void *p1, U64 cb1, struct fs_req *q)
{
	U64 h, m[2];
	U32 nsend = (code == FS_READB || code == FS_READBLK || code == FS_DIRSEC) ? 0 : 1;
	erc_t erc;

	erc = Request("FILESYS ", code, current->exch, &h, nsend, (U8 *)p1, cb1, (U8 *)q, sizeof(*q),
		      0, 0, 0);
	if (erc)
		return erc;
	if ((erc = WaitMsg(current->exch, m)))
		return erc;
	return (erc_t)m[1];
}

erc_t OpenFile(const char *pName, U32 cbName, U32 Mode, U32 fStream, U32 *pdHandleRet)
{
	struct fs_req q = { .mode = Mode, .stream = fStream };
	erc_t erc = call(FS_OPEN, pName, cbName, &q);

	if (!erc)
		*pdHandleRet = q.out32;
	return erc;
}

erc_t CloseFile(U32 dHandle)
{
	struct fs_req q = { .handle = dHandle };

	return call(FS_CLOSE, NULL, 0, &q);
}

erc_t CreateFile(const char *pName, U32 cbName, U32 Attrib)
{
	struct fs_req q = { .attr = Attrib };

	return call(FS_CREATE, pName, cbName, &q);
}

erc_t DeleteFile(U32 dHandle)
{
	struct fs_req q = { .handle = dHandle };

	return call(FS_DELETE, NULL, 0, &q);
}

erc_t RenameFile(const char *pCrntName, U32 cbCrntName, const char *pNewName, U32 cbNewName)
{
	struct fs_req q;

	if (cbNewName >= MAX_PATH)
		return ErcBadFileSpec;
	memset(&q, 0, sizeof(q));
	memcpy(q.name2, pNewName, cbNewName);
	q.name2_len = cbNewName;
	return call(FS_RENAME, pCrntName, cbCrntName, &q);
}

erc_t CreateDir(const char *pName, U32 cbName)
{
	struct fs_req q;

	memset(&q, 0, sizeof(q));
	return call(FS_MKDIR, pName, cbName, &q);
}

erc_t DeleteDir(const char *pName, U32 cbName)
{
	struct fs_req q;

	memset(&q, 0, sizeof(q));
	return call(FS_RMDIR, pName, cbName, &q);
}

erc_t ReadBytes(U32 dHandle, U8 *pBytesRet, U64 nBytes, U64 *pdBytesRet)
{
	struct fs_req q = { .handle = dHandle, .n = nBytes };
	erc_t erc = call(FS_READB, pBytesRet, nBytes, &q);

	*pdBytesRet = q.out;
	return erc;
}

erc_t WriteBytes(U32 dHandle, const U8 *pBytes, U64 nBytes, U64 *pdBytesRet)
{
	struct fs_req q = { .handle = dHandle, .n = nBytes };
	erc_t erc = call(FS_WRITEB, pBytes, nBytes, &q);

	*pdBytesRet = q.out;
	return erc;
}

erc_t ReadBlock(U32 dHandle, U8 *pBytesRet, U64 nBytes, U64 dLFA, U64 *pdnBytesRet)
{
	struct fs_req q = { .handle = dHandle, .n = nBytes, .lfa = dLFA };
	erc_t erc = call(FS_READBLK, pBytesRet, nBytes, &q);

	*pdnBytesRet = q.out;
	return erc;
}

erc_t WriteBlock(U32 dHandle, const U8 *pData, U64 nBytes, U64 dLFA, U64 *pdnBytesRet)
{
	struct fs_req q = { .handle = dHandle, .n = nBytes, .lfa = dLFA };
	erc_t erc = call(FS_WRITEBLK, pData, nBytes, &q);

	*pdnBytesRet = q.out;
	return erc;
}

erc_t GetFileSize(U32 dHandle, U64 *pdSizeRet)
{
	struct fs_req q = { .handle = dHandle };
	erc_t erc = call(FS_GETSIZE, NULL, 0, &q);

	*pdSizeRet = q.out;
	return erc;
}

erc_t SetFileSize(U32 dHandle, U64 dSize)
{
	struct fs_req q = { .handle = dHandle, .n = dSize };

	return call(FS_SETSIZE, NULL, 0, &q);
}

erc_t GetFileLFA(U32 dHandle, U64 *pdLFARet)
{
	struct fs_req q = { .handle = dHandle };
	erc_t erc = call(FS_GETLFA, NULL, 0, &q);

	*pdLFARet = q.out;
	return erc;
}

erc_t SetFileLFA(U32 dHandle, U64 dLFA)
{
	struct fs_req q = { .handle = dHandle, .lfa = dLFA };

	return call(FS_SETLFA, NULL, 0, &q);
}

erc_t GetDirSector(const char *pPathSpec, U32 cbPathSpec, U8 *pSectorRet, U32 sSectorRet,
		   U32 SectorNum)
{
	struct fs_req q;

	if (cbPathSpec >= MAX_PATH)
		return ErcBadFileSpec;
	if (sSectorRet < 512)
		return ErcBadMemReq;
	memset(&q, 0, sizeof(q));
	memcpy(q.name2, pPathSpec, cbPathSpec);
	q.name2_len = cbPathSpec;
	q.sectnum = SectorNum;
	return call(FS_DIRSEC, pSectorRet, 512, &q);
}

erc_t GetSystemDisk(U8 *pDiskRet)
{
	*pDiskRet = vol_get('C') ? 2 : 0;	/* MMURTL: 0 = A, 2 = C */
	return ErcOk;
}
