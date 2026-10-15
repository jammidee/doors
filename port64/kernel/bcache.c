/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Block cache: 512-byte sectors, LRU, write-through (a write goes to the disk before the call
   returns, so the disk is always consistent; a write-back cache can come with journaling, M8).
   Used by the file system service (one task); DeviceOp writes invalidate what they touch. */

#include <doors/blk.h>
#include <doors/console.h>
#include <doors/mem.h>
#include <doors/string.h>
#include <doors/x86.h>

#define NBUF 1024				/* 512 KB of sectors */

struct cbuf {
	struct blkdev *dev;
	U64 lba;
	U64 used;				/* LRU stamp */
	bool valid;
};

static struct cbuf *meta;
static U8 *data;
static U64 clock, hits, misses;

void bc_init(void)
{
	U8 *p;

	if (AllocOSPage((U32)(PAGE_ROUND_UP(NBUF * sizeof(struct cbuf)) / PAGE_SIZE), &p))
		panic("bcache: no memory");
	meta = (struct cbuf *)p;
	if (AllocOSPage(NBUF * 512 / PAGE_SIZE, &data))
		panic("bcache: no memory");
}

static struct cbuf *find(struct blkdev *d, U64 lba)
{
	U32 i;

	for (i = 0; i < NBUF; i++)
		if (meta[i].valid && meta[i].dev == d && meta[i].lba == lba)
			return &meta[i];
	return NULL;
}

static struct cbuf *victim(void)
{
	struct cbuf *v = &meta[0];
	U32 i;

	for (i = 0; i < NBUF; i++) {
		if (!meta[i].valid)
			return &meta[i];
		if (meta[i].used < v->used)
			v = &meta[i];
	}
	return v;
}

static U8 *bytes(struct cbuf *b)
{
	return data + (U64)(b - meta) * 512;
}

erc_t bc_read(struct blkdev *d, U64 lba, void *dst)
{
	struct cbuf *b = find(d, lba);
	erc_t erc;

	if (b) {
		hits++;
	} else {
		misses++;
		b = victim();
		b->valid = false;
		if ((erc = blk_io(d, CmdRead, lba, 1, bytes(b))))
			return erc;
		b->dev = d;
		b->lba = lba;
		b->valid = true;
	}
	b->used = ++clock;
	memcpy(dst, bytes(b), 512);
	return ErcOk;
}

erc_t bc_write(struct blkdev *d, U64 lba, const void *src)
{
	struct cbuf *b = find(d, lba);
	erc_t erc;

	if (!b) {
		b = victim();
		b->valid = false;
	}
	memcpy(bytes(b), src, 512);
	if ((erc = blk_io(d, CmdWrite, lba, 1, bytes(b))))
		return erc;
	b->dev = d;
	b->lba = lba;
	b->valid = true;
	b->used = ++clock;
	return ErcOk;
}

void bc_invalidate(struct blkdev *d, U64 lba, U32 n)
{
	U32 i;

	if (!meta)
		return;
	for (i = 0; i < NBUF; i++)
		if (meta[i].valid && meta[i].dev == d && meta[i].lba >= lba && meta[i].lba < lba + n)
			meta[i].valid = false;
}

void bc_stats(U64 *h, U64 *m)
{
	*h = hits;
	*m = misses;
}
