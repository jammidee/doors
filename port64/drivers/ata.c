/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* ATA (IDE) hard disk driver, PIO, LBA48 (LBA28 if the drive lacks it).

   Replaces MMURTL's HARDIDE.C. Addressing is by LBA only, with the size from IDENTIFY DEVICE,
   so there is no CHS geometry to get wrong (the 32-bit baseline needed a hand-made disk to work
   around FSYS.C taking the head count from the partition table, see devlogs/2026-10-16.md).

   Status is polled (device interrupts are disabled with nIEN); a channel is used by one task at
   a time (kmutex). Positions: primary master/slave = devices 12/13 (MMURTL hard disk 0/1),
   secondary master/slave = 14/15. ATAPI devices (the boot CD) are skipped. */

#include <doors/blk.h>
#include <doors/console.h>
#include <doors/cpu.h>
#include <doors/msg.h>
#include <doors/string.h>
#include <doors/task.h>
#include <doors/x86.h>

#define REG_DATA    0
#define REG_ERR     1
#define REG_COUNT   2
#define REG_LBA0    3
#define REG_LBA1    4
#define REG_LBA2    5
#define REG_DRIVE   6
#define REG_CMD     7	/* write */
#define REG_STATUS  7	/* read */

#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_DF   0x20
#define ST_BSY  0x80

#define CMD_READ_PIO      0x20
#define CMD_READ_PIO_EXT  0x24
#define CMD_WRITE_PIO     0x30
#define CMD_WRITE_PIO_EXT 0x34
#define CMD_FLUSH         0xE7
#define CMD_FLUSH_EXT     0xEA
#define CMD_IDENTIFY      0xEC

#define TIMEOUT_TICKS 300	/* 3 s */
#define FLUSH_TICKS   3000	/* 30 s: a cache flush may take that long (ATA spec); in QEMU it is an
				   fsync of the image file on the host */

struct channel {
	U16 base, ctrl;
	struct kmutex lock;
	bool lock_ready;
};

struct ata {
	struct blkdev dev;
	struct channel *ch;
	U8 slave;
	bool lba48;
};

static struct channel chans[2] = { { 0x1F0, 0x3F6, { 0 }, false }, { 0x170, 0x376, { 0 }, false } };
static struct ata drives[4];

static void delay400(struct channel *c)
{
	int i;

	for (i = 0; i < 4; i++)
		inb(c->ctrl);		/* each alternate-status read takes ~100 ns */
}

static bool panicking;		/* ata_panic_io: interrupts are off, the timer doesn't tick */

/* Wait until BSY is clear, then for `want` bits (DRQ) unless the drive reports an error. */
static erc_t wait_t(struct channel *c, U8 want, U64 timeout)
{
	U64 end = timer_ticks() + timeout, spins = 0;
	U8 st;

	for (;;) {
		st = inb(c->base + REG_STATUS);
		if (!(st & ST_BSY)) {
			if (st & (ST_ERR | ST_DF))
				return ErcDevError;
			if ((st & want) == want)
				return ErcOk;
		}
		if (panicking ? ++spins > timeout * 100000 : timer_ticks() > end)
			return ErcDevTimeOut;
	}
}

static erc_t wait(struct channel *c, U8 want)
{
	return wait_t(c, want, TIMEOUT_TICKS);
}

static void select(struct ata *a, U64 lba, U32 n)
{
	struct channel *c = a->ch;

	if (a->lba48) {
		outb(c->base + REG_DRIVE, (U8)(0x40 | (a->slave << 4)));
		delay400(c);
		outb(c->base + REG_COUNT, (U8)(n >> 8));
		outb(c->base + REG_LBA0, (U8)(lba >> 24));
		outb(c->base + REG_LBA1, (U8)(lba >> 32));
		outb(c->base + REG_LBA2, (U8)(lba >> 40));
		outb(c->base + REG_COUNT, (U8)n);
		outb(c->base + REG_LBA0, (U8)lba);
		outb(c->base + REG_LBA1, (U8)(lba >> 8));
		outb(c->base + REG_LBA2, (U8)(lba >> 16));
	} else {
		outb(c->base + REG_DRIVE, (U8)(0xE0 | (a->slave << 4) | ((lba >> 24) & 0x0F)));
		delay400(c);
		outb(c->base + REG_COUNT, (U8)n);
		outb(c->base + REG_LBA0, (U8)lba);
		outb(c->base + REG_LBA1, (U8)(lba >> 8));
		outb(c->base + REG_LBA2, (U8)(lba >> 16));
	}
}

static erc_t xfer(struct ata *a, U32 op, U64 lba, U32 n, U8 *buf)
{
	struct channel *c = a->ch;
	U32 i;
	erc_t erc;

	if ((erc = wait(c, 0)))
		return erc;
	select(a, lba, n);
	if (op == CmdWrite)
		outb(c->base + REG_CMD, a->lba48 ? CMD_WRITE_PIO_EXT : CMD_WRITE_PIO);
	else
		outb(c->base + REG_CMD, a->lba48 ? CMD_READ_PIO_EXT : CMD_READ_PIO);

	for (i = 0; i < n; i++, buf += 512) {
		U16 *w = (U16 *)buf;
		U32 cnt = 256;

		delay400(c);
		if ((erc = wait(c, ST_DRQ)))
			return erc;
		if (op == CmdWrite)
			__asm__ volatile("rep outsw" : "+S"(w), "+c"(cnt) : "d"(c->base) : "memory");
		else
			__asm__ volatile("rep insw" : "+D"(w), "+c"(cnt) : "d"(c->base) : "memory");
	}
	if (op == CmdWrite) {
		outb(c->base + REG_CMD, a->lba48 ? CMD_FLUSH_EXT : CMD_FLUSH);
		delay400(c);
		if ((erc = wait_t(c, 0, FLUSH_TICKS)))
			return erc;
	}
	return ErcOk;
}

/* From panic (a crash report): no lock (the panic may have stopped a task holding it), the waits
   counted in loops, and a software reset first in case a transfer was cut off in the middle. */
static erc_t ata_panic_io(struct blkdev *d, U32 op, U64 lba, U32 n, U8 *buf)
{
	struct ata *a = d->priv;
	struct channel *c = a->ch;
	U32 i;

	panicking = true;
	outb(c->ctrl, 0x06);			/* SRST + nIEN */
	for (i = 0; i < 50; i++)
		delay400(c);
	outb(c->ctrl, 0x02);			/* nIEN: polled, as always */
	for (i = 0; i < 200; i++)
		delay400(c);
	return op == CmdWrite || op == CmdRead ? xfer(a, op, lba, n, buf) : ErcBadOpNum;
}

static erc_t ata_io(struct blkdev *d, U32 op, U64 lba, U32 n, U8 *buf)
{
	struct ata *a = d->priv;
	U8 tmp[512];
	erc_t erc = ErcOk;
	U32 max = a->lba48 ? 128 : 128;

	if (!a->ch->lock_ready) {
		kmutex_init(&a->ch->lock);
		a->ch->lock_ready = true;
	}
	kmutex_lock(&a->ch->lock);
	while (n && !erc) {
		U32 k = n < max ? n : max;

		if (op == CmdVerify) {		/* read and discard */
			U32 i;

			for (i = 0; i < k && !erc; i++)
				erc = xfer(a, CmdRead, lba + i, 1, tmp);
		} else {
			erc = xfer(a, op, lba, k, buf);
			buf += (U64)k * 512;
		}
		lba += k;
		n -= k;
	}
	kmutex_unlock(&a->ch->lock);
	return erc;
}

static void identify(struct ata *a)
{
	struct channel *c = a->ch;
	U16 id[256] = { 0 };			/* (filled by rep insw, which analyzers can't see) */
	U32 cnt = 256;
	U16 *w = id;
	U8 st;
	int i;

	outb(c->base + REG_DRIVE, (U8)(0xA0 | (a->slave << 4)));
	delay400(c);
	if (inb(c->base + REG_STATUS) == 0xFF)
		return;				/* floating bus: no channel */
	outb(c->base + REG_COUNT, 0);
	outb(c->base + REG_LBA0, 0);
	outb(c->base + REG_LBA1, 0);
	outb(c->base + REG_LBA2, 0);
	outb(c->base + REG_CMD, CMD_IDENTIFY);
	delay400(c);
	if (!inb(c->base + REG_STATUS))
		return;				/* no drive */
	if (wait(c, 0) == ErcDevTimeOut)
		return;
	if (inb(c->base + REG_LBA1) || inb(c->base + REG_LBA2))
		return;				/* ATAPI (CD) or SATA signature: not ours */
	st = (U8)wait(c, ST_DRQ);
	if (st)
		return;
	__asm__ volatile("rep insw" : "+D"(w), "+c"(cnt) : "d"(c->base) : "memory");

	a->lba48 = id[83] & (1 << 10);
	if (a->lba48)
		a->dev.nblocks = (U64)id[100] | ((U64)id[101] << 16) | ((U64)id[102] << 32) |
				 ((U64)id[103] << 48);
	else
		a->dev.nblocks = (U64)id[60] | ((U64)id[61] << 16);
	if (!a->dev.nblocks)
		return;

	/* model string: words 27-46, byte-swapped; as much as fits, with room for the NUL (the old
	   loop checked after writing and filled all 24 bytes: found by cppcheck) */
	memset(a->dev.name, 0, sizeof(a->dev.name));
	for (i = 0; i < (int)(sizeof(a->dev.name) - 1) / 2; i++) {	/* 11 words, 22 characters */
		a->dev.name[2 * i] = (char)(id[27 + i] >> 8);
		a->dev.name[2 * i + 1] = (char)id[27 + i];
	}
	for (i = (int)sizeof(a->dev.name) - 2; i >= 0 && (a->dev.name[i] == ' ' || !a->dev.name[i]); i--)
		a->dev.name[i] = 0;
	a->dev.io = ata_io;
	a->dev.panic_io = ata_panic_io;
	a->dev.priv = a;
	if (blk_register(&a->dev) == ErcOk)
		kprintf("Disk %u: ATA %s %s, %lu MB%s ('%s')\n", a->dev.devnum,
			a->ch == &chans[0] ? "primary" : "secondary", a->slave ? "slave" : "master",
			a->dev.nblocks >> 11, a->lba48 ? ", LBA48" : "", a->dev.name);
}

void ata_init(void)
{
	U32 i;

	for (i = 0; i < 2; i++)
		outb(chans[i].ctrl, 0x02);	/* nIEN: no interrupts, we poll */
	for (i = 0; i < 4; i++) {
		drives[i].ch = &chans[i / 2];
		drives[i].slave = (U8)(i & 1);
		drives[i].dev.devnum = DEV_HD0 + i;
		identify(&drives[i]);
	}
}
