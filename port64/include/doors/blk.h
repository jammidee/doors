/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* DOORS block devices and the MMURTL device driver interface (OSSOURCE/DEVDRVR.ASM, MDEVDRV.H).

   Device numbers follow MMURTL: 10/11 = floppy A/B, 12/13 = hard disk 0/1 (here: the ATA primary
   master/slave), 14/15 = ATA secondary master/slave. DeviceOp keeps MMURTL's parameter order;
   the LBA is 64-bit. Operations: 1 = read, 2 = write, 3 = verify (MMURTL CmdRead/CmdWrite/CmdVerify). */
#ifndef DOORS_BLK_H
#define DOORS_BLK_H

#include <doors/erc.h>
#include <doors/types.h>

#define CmdRead   1
#define CmdWrite  2
#define CmdVerify 3

#define MAX_DEVS  32
#define DEV_HD0   12

struct blkdev {
	U32 devnum;
	char name[24];
	U64 nblocks;			/* 512-byte blocks */
	erc_t (*io)(struct blkdev *d, U32 op, U64 lba, U32 n, U8 *buf);
	void *priv;
};

/* What DeviceStat returns (DOORS layout; MMURTL's was per driver). */
struct devstat {
	U32 devnum;
	U32 block_size;
	U64 nblocks;
	char name[24];
};

erc_t blk_register(struct blkdev *d);
struct blkdev *blk_get(U32 devnum);
erc_t blk_io(struct blkdev *d, U32 op, U64 lba, U32 n, U8 *buf);	/* bounds-checked */

/* ---- MMURTL calls ---- */
erc_t DeviceOp(U32 dDevice, U32 dOpNum, U64 dLBA, U32 dnBlocks, U8 *pData);
erc_t DeviceStat(U32 dDevice, U8 *pStatRet, U32 dStatusMax, U32 *pdStatusRet);
erc_t DeviceInit(U32 dDevNum, U8 *pInitData, U32 sdInitData);

/* ---- drivers ---- */
void ata_init(void);			/* probes the 4 ATA positions, registers 12..15 */

/* ---- block cache (kernel/bcache.c), 512-byte sectors, write-through ---- */
void  bc_init(void);
erc_t bc_read(struct blkdev *d, U64 lba, void *dst);
erc_t bc_write(struct blkdev *d, U64 lba, const void *src);
erc_t bc_write_n(struct blkdev *d, U64 lba, U32 n, const void *src);	/* one device command */
void  bc_invalidate(struct blkdev *d, U64 lba, U32 n);
void  bc_stats(U64 *hits, U64 *misses);

/* ---- CMOS real-time clock (drivers/rtc.c) ---- */
struct rtc_time {
	U16 year;
	U8 month, day, hour, min, sec;
};
void rtc_read(struct rtc_time *t);
U32  days_from_civil(U32 y, U32 m, U32 d);	/* days since 1970-01-01 */
U32  unix_time(void);
erc_t GetCMOSTime(U32 *pdTimeRet);		/* MMURTL: BCD seconds, minutes, hours */
erc_t GetCMOSDate(U32 *pdDateRet);		/* MMURTL: BCD day of week, day, month, year */

#endif
