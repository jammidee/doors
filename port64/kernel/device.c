/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Device table and the MMURTL device calls DeviceOp / DeviceStat / DeviceInit (DEVDRVR.ASM). */

#include <doors/blk.h>
#include <doors/string.h>
#include <doors/x86.h>

static struct blkdev *devs[MAX_DEVS];

erc_t blk_register(struct blkdev *d)
{
	if (d->devnum >= MAX_DEVS)
		return ErcBadDevNum;
	if (devs[d->devnum])
		return ErcDevInUse;
	devs[d->devnum] = d;
	return ErcOk;
}

struct blkdev *blk_get(U32 devnum)
{
	return devnum < MAX_DEVS ? devs[devnum] : NULL;
}

erc_t blk_io(struct blkdev *d, U32 op, U64 lba, U32 n, U8 *buf)
{
	if (op != CmdRead && op != CmdWrite && op != CmdVerify)
		return ErcBadOpNum;
	if (!n || lba >= d->nblocks || n > d->nblocks - lba)
		return ErcBeyondEOF;
	return d->io(d, op, lba, n, buf);
}

erc_t DeviceOp(U32 dDevice, U32 dOpNum, U64 dLBA, U32 dnBlocks, U8 *pData)
{
	struct blkdev *d;
	erc_t erc;

	if (dDevice >= MAX_DEVS)
		return ErcBadDevNum;
	d = devs[dDevice];
	if (!d)
		return ErcNoDriver;
	erc = blk_io(d, dOpNum, dLBA, dnBlocks, pData);
	if (dOpNum == CmdWrite)
		bc_invalidate(d, dLBA, dnBlocks);	/* raw writes bypass the cache */
	return erc;
}

erc_t DeviceStat(U32 dDevice, U8 *pStatRet, U32 dStatusMax, U32 *pdStatusRet)
{
	struct devstat s;
	struct blkdev *d;
	U32 n;

	if (dDevice >= MAX_DEVS)
		return ErcBadDevNum;
	d = devs[dDevice];
	if (!d)
		return ErcNoDriver;
	memset(&s, 0, sizeof(s));
	s.devnum = d->devnum;
	s.block_size = 512;
	s.nblocks = d->nblocks;
	memcpy(s.name, d->name, sizeof(s.name));
	n = dStatusMax < sizeof(s) ? dStatusMax : (U32)sizeof(s);
	memcpy(pStatRet, &s, n);
	*pdStatusRet = n;
	return ErcOk;
}

erc_t DeviceInit(U32 dDevNum, U8 *pInitData, U32 sdInitData)
{
	(void)pInitData;
	(void)sdInitData;
	if (dDevNum >= MAX_DEVS)
		return ErcBadDevNum;
	return devs[dDevNum] ? ErcOk : ErcNoDriver;	/* drivers configure themselves (LBA) */
}
