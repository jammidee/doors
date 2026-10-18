/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* DOORS file system: volumes (partitions with a drive letter), the FAT driver, and the MMURTL
   file calls (MFILES.H) with the same names and parameter order. Sizes, offsets (LFA) and byte
   counts are 64-bit, so files larger than 4 GB fit the API.

   As in MMURTL the file system is a system service ("FILESYS "): one kernel task serves all
   file requests in order, and the calls below send it a Request and wait for the response.
   User buffers reach it through the request's aliasing, like any other service. */
#ifndef DOORS_FS_H
#define DOORS_FS_H

#include <doors/dirent.h>
#include <doors/erc.h>
#include <doors/types.h>

#define ModeRead   0
#define ModeModify 1

#define MAX_VOLS    8
#define MAX_PATH    260

struct blkdev;
struct fat;

enum vol_kind { VOL_NONE, VOL_FAT12, VOL_FAT16, VOL_FAT32, VOL_LINUX, VOL_EXT4 };

struct volume {
	char letter;			/* 'C', 'D', ... */
	struct blkdev *dev;
	U64 start, nsectors;		/* partition, in 512-byte sectors */
	U8 kind;			/* enum vol_kind */
	U8 part;			/* partition number (1-based, 0 = whole disk) */
	struct fat *fat;		/* mounted FAT, if any */
	struct ext4 *ext4;		/* mounted ext4, if any */
	const struct fs_ops *ops;	/* NULL: not mounted */
	bool ro;			/* mounted read-only */
	char label[17];
};

void vol_init(void);			/* scan disks, partitions; mount FAT volumes */
struct volume *vol_get(char letter);
struct volume *vol_index(U32 i);

/* FAT driver (kernel/fat.c), used by the FILESYS service only */
erc_t fat_mount(struct volume *v);	/* a volume left dirty is checked (chkdsk-style report) */
void  fat_unmount(void);		/* clear the dirty flag of the volumes DOORS wrote (clean) */
bool  fat_bpb_ok(const U8 *sector);	/* plausible FAT boot sector (BPB)? */

/* Internal FAT operations behind the FILESYS service (paths are "C:\DIR\NAME"). */
struct fnode {			/* a file or directory as found by a lookup */
	struct volume *vol;
	U64 key;			/* identity on its volume (open-file sharing rules) */
	U64 size;
	U8 attr;			/* ATTR_DIR, ATTR_RO ... */
	bool is_root;
	U32 mtime;			/* Unix time (ReadDir) */
	/* FAT */
	U32 dir_clus;			/* directory holding it (0 = FAT12/16 fixed root) */
	U32 idx;			/* index of its 8.3 entry in that directory */
	U32 lfn_idx, lfn_n;		/* its long-name entries (lfn_n = 0: none) */
	U32 first;			/* first cluster */
	/* ext4 */
	U32 ino;			/* inode number */
	U32 dir_ino;			/* directory holding its name */
	char name[256];			/* that name (ext4 needs it to unlink/rename) */
};

/* What a file system provides to the FILESYS service. Paths include the drive ("C:\..."). */
struct fs_ops {
	const char *name;
	erc_t (*lookup)(const char *path, U32 len, struct fnode *n);
	erc_t (*create)(const char *path, U32 len, U8 attr, struct fnode *n);
	erc_t (*remove)(struct fnode *n);
	erc_t (*rename)(struct fnode *n, const char *newpath, U32 len);
	erc_t (*read)(struct fnode *n, U64 off, U8 *buf, U64 len, U64 *done);
	erc_t (*write)(struct fnode *n, U64 off, const U8 *buf, U64 len, U64 *done);
	erc_t (*truncate)(struct fnode *n, U64 size);
	erc_t (*readdir)(struct fnode *dir, U32 index, struct dir_entry *out);
	bool  (*dir_empty)(struct fnode *dir);
	erc_t (*dir_sector)(struct fnode *dir, U32 sectnum, U8 *out);	/* FAT only (GetDirSector) */
};

extern const struct fs_ops fat_ops, ext4_ops;
erc_t ext4_mount(struct volume *v);
U32   crc32c(U32 crc, const void *buf, U64 len);	/* raw CRC32C update (no inversions), as ext4 uses */
bool  crc32c_selftest(void);

erc_t fat_lookup(const char *path, U32 len, struct fnode *n);
erc_t fat_create(const char *path, U32 len, U8 attr, struct fnode *n);
erc_t fat_remove(struct fnode *n);		/* frees clusters and the entries */
erc_t fat_rename(struct fnode *n, const char *newpath, U32 len);
erc_t fat_read(struct fnode *n, U64 off, U8 *buf, U64 len, U64 *done);
erc_t fat_write(struct fnode *n, U64 off, const U8 *buf, U64 len, U64 *done);
erc_t fat_truncate(struct fnode *n, U64 size);
erc_t fat_update(struct fnode *n);		/* write size/first cluster/time back */
erc_t fat_dir_sector(struct fnode *dir, U32 sectnum, U8 *out);
bool  fat_dir_empty(struct fnode *dir);

/* service (kernel/fsys.c) */
void fsys_init(void);			/* starts the FILESYS service task */
U32  fsys_open_count(void);
void fsys_job_gone(U32 job);		/* close a dead job's files */

/* ---- MMURTL file calls (MFILES.H) ---- */
erc_t OpenFile(const char *pName, U32 cbName, U32 Mode, U32 fStream, U32 *pdHandleRet);
erc_t CloseFile(U32 dHandle);
erc_t CreateFile(const char *pName, U32 cbName, U32 Attrib);
erc_t DeleteFile(U32 dHandle);
erc_t RenameFile(const char *pCrntName, U32 cbCrntName, const char *pNewName, U32 cbNewName);
erc_t CreateDir(const char *pName, U32 cbName);
erc_t DeleteDir(const char *pName, U32 cbName);
erc_t ReadBytes(U32 dHandle, U8 *pBytesRet, U64 nBytes, U64 *pdBytesRet);
erc_t WriteBytes(U32 dHandle, const U8 *pBytes, U64 nBytes, U64 *pdBytesRet);
erc_t ReadBlock(U32 dHandle, U8 *pBytesRet, U64 nBytes, U64 dLFA, U64 *pdnBytesRet);
erc_t WriteBlock(U32 dHandle, const U8 *pData, U64 nBytes, U64 dLFA, U64 *pdnBytesRet);
erc_t GetFileSize(U32 dHandle, U64 *pdSizeRet);
erc_t SetFileSize(U32 dHandle, U64 dSize);
erc_t GetFileLFA(U32 dHandle, U64 *pdLFARet);
erc_t SetFileLFA(U32 dHandle, U64 dLFA);	/* 0xFFFF...FF = end of file */
erc_t GetDirSector(const char *pPathSpec, U32 cbPathSpec, U8 *pSectorRet, U32 sSectorRet,
		   U32 SectorNum);
erc_t GetSystemDisk(U8 *pDiskRet);		/* 0 = A, 1 = B, 2 = C ... */

/* DOORS addition (system call 110, after MMURTL's): entry dIndex of a directory, any file system.
   '.' and '..' are not returned. Past the last entry: ErcNoMatch. */
erc_t ReadDir(const char *pPath, U32 cbPath, U32 dIndex, struct dir_entry *pEntryRet);

/* power-cut test (kernel/powercut.c): halt inside the K-th journal commit when set */
extern U32 ext4_commit_stop;
void  ext4_unmount(void);			/* clear needs_recovery (journal empty): a clean shutdown */
NORETURN void powercut_run(const char *cmdline);
NORETURN void powercut_check(const char *cmdline);

#endif
