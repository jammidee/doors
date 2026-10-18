/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Block cache: 512-byte sectors, LRU, write-through (a write goes to the disk before the call
   returns, so the disk is always consistent; a write-back cache can come with journaling, M8).
   Used by the file system service (one task); DeviceOp writes invalidate what they touch. */

#include <doors/blk.h>
#include <doors/console.h>
#include <doors/mem.h>
#include <doors/string.h>
#include <doors/x86.h>

#define NBUF  1024				/* 512 KB of sectors */
#define NHASH 2048				/* hash chains over (device, sector) */

struct cbuf {
	struct blkdev *dev;
	U64 lba;
	U64 used;				/* LRU stamp */
	bool valid;				/* valid buffers, and only they, are in a hash chain */
	U16 hnext;				/* next in the chain: index + 1, 0 = end */
};

static struct cbuf *meta;
static U8 *data;
static U16 hhead[NHASH];			/* first buffer of each chain: index + 1 */
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

static U32 hash(struct blkdev *d, U64 lba)
{
	return (U32)((lba * 0x9E3779B97F4A7C15ULL ^ (U64)(uintptr_t)d) >> 40) & (NHASH - 1);
}

static struct cbuf *find(struct blkdev *d, U64 lba)
{
	U32 i;

	for (i = hhead[hash(d, lba)]; i; i = meta[i - 1].hnext)
		if (meta[i - 1].dev == d && meta[i - 1].lba == lba)
			return &meta[i - 1];
	return NULL;
}

/* b->dev and b->lba set: mark it valid and put it in its chain */
static void hash_in(struct cbuf *b)
{
	U16 *h = &hhead[hash(b->dev, b->lba)];

	b->valid = true;
	b->hnext = *h;
	*h = (U16)(b - meta + 1);
}

/* make b invalid, taking it out of its chain */
static void unhash(struct cbuf *b)
{
	U16 *p, me = (U16)(b - meta + 1);

	if (!b->valid)
		return;
	for (p = &hhead[hash(b->dev, b->lba)]; *p; p = &meta[*p - 1].hnext)
		if (*p == me) {
			*p = b->hnext;
			break;
		}
	b->valid = false;
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
		unhash(b);
		if ((erc = blk_io(d, CmdRead, lba, 1, bytes(b))))
			return erc;
		b->dev = d;
		b->lba = lba;
		hash_in(b);
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
		unhash(b);
	}
	memcpy(bytes(b), src, 512);
	if ((erc = blk_io(d, CmdWrite, lba, 1, bytes(b))))
		return erc;
	if (!b->valid) {
		b->dev = d;
		b->lba = lba;
		hash_in(b);
	}
	b->used = ++clock;
	return ErcOk;
}

/* n consecutive sectors in one device command (one ATA write and one flush instead of n of each),
   then into the cache like bc_write. */
erc_t bc_write_n(struct blkdev *d, U64 lba, U32 n, const void *src)
{
	const U8 *s = src;
	U32 i;
	erc_t erc;

	if (n == 1)
		return bc_write(d, lba, src);
	if ((erc = blk_io(d, CmdWrite, lba, n, (U8 *)(uintptr_t)s)))
		return erc;
	for (i = 0; i < n; i++) {
		struct cbuf *b = find(d, lba + i);

		if (!b) {
			b = victim();
			unhash(b);
			b->dev = d;
			b->lba = lba + i;
			hash_in(b);
		}
		memcpy(bytes(b), s + (U64)i * 512, 512);
		b->used = ++clock;
	}
	return ErcOk;
}

void bc_invalidate(struct blkdev *d, U64 lba, U32 n)
{
	U32 i;

	if (!meta)
		return;
	for (i = 0; i < NBUF; i++)
		if (meta[i].valid && meta[i].dev == d && meta[i].lba >= lba && meta[i].lba < lba + n)
			unhash(&meta[i]);
}

void bc_stats(U64 *h, U64 *m)
{
	*h = hits;
	*m = misses;
}
