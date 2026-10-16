<!-- DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. -->
# 6. File system

## Layers

```
 program           OpenFile, ReadBytes, ...  (MMURTL MFILES.H API, 64-bit sizes)
    |  Request to "FILESYS "
 FILESYS service   kernel/fsys.c   handles, modes, sharing, one request at a time (priority 5)
    |  struct fs_ops (include/doors/fs.h)
 FAT  |  ext4      kernel/fat.c, kernel/ext4.c
    |
 volumes           kernel/vol.c    partitions -> drive letters
 block cache       kernel/bcache.c 1024 x 512-byte sectors, write-through
 disk driver       drivers/ata.c   ATA PIO, LBA48, devices 12-15
```

As in MMURTL, the file system is a **service**. Each file call builds a request to `"FILESYS "`,
waits for the response on the caller's own exchange, and returns its status. User buffers reach
the service through request aliasing (see [chapter 5](05-messaging.md)).

## Disks, partitions and drive letters

At boot DOORS looks at every ATA disk (device numbers 12–15) and reads its partition table:

- **MBR**: the four primary partitions;
- **GPT**: the partition entries (supported, not yet covered by a test);
- **no partition table**: a disk that starts with a FAT boot sector is one volume ("superfloppy").

Partitions get drive letters in order, **starting at C:**, with up to 8 volumes. A Linux partition
that DOORS can't mount is reported at boot as `Linux (not mounted)`, and file calls on it return
`ErcNotSupported`.

## Paths

- A full path starts with a drive letter: `C:\DIR1\SUB\DEEP.TXT`.
- Each job has a **path** (`SetPath`, the CLI's `path` command), as in MMURTL's FSYS.C `BuildSpec`.
  A name without a drive is added to it: with the path `C:\DOORS\`, `HELP.CLI` means
  `C:\DOORS\HELP.CLI`. A name that starts with `\` gets only the path's drive: `\BIN\HELLO.ELF` means
  `C:\BIN\HELLO.ELF`. An empty name means the path itself (`ReadDir` of `""` lists it). Without a
  path (job 1, or a program that never set one), names need a drive.
- `\` and `/` both separate names. `.` and `..` work on both file systems.
- Names may be long and contain spaces: `C:\A Long File Name.txt`.
- The longest path is 259 characters (`MAX_PATH` 260).
- Paths are passed as `(pointer, length)` pairs, as in MMURTL. They need no terminating NUL.
- **FAT** names ignore case (`c:\hello.txt` finds `HELLO.TXT`). **ext4** names are
  case-sensitive (`Case.txt` and `case.txt` are two files).

## Opening files

```c
erc_t OpenFile(const char *pName, U32 cbName, U32 Mode, U32 fStream, U32 *pdHandleRet);
```

**Mode** (as in MMURTL):

| Mode | Value | Sharing |
|---|---|---|
| `ModeRead` | 0 | any number of readers at the same time |
| `ModeModify` | 1 | exclusive: fails with `ErcFileInUse` if the file is open in any mode |

A file with the read-only attribute, or a file on a read-only volume, can't be opened with
`ModeModify` (`ErcReadOnly`). Opening a directory gives `ErcNotAFile`.

**fStream** chooses how the file is read and written:

| | Stream file (`fStream` ≠ 0) | Block file (`fStream` = 0) |
|---|---|---|
| Calls | `ReadBytes`, `WriteBytes` | `ReadBlock`, `WriteBlock` |
| Position | the handle's current LFA (logical file address), advanced by each call | given in each call (`dLFA`) |
| Sizes | any number of bytes | `dLFA` and the byte count must be multiples of 512 (`ErcBadFileSpec` otherwise) |
| Wrong call | `ErcBlockFile` | `ErcStreamFile` |

Handles belong to the job that opened them. Up to 128 files are open at a time in the whole
system. When a job ends, its files are closed.

## The file calls

All sizes and positions are 64-bit, so files larger than 4 GB work.

| Call | What it does |
|---|---|
| `CreateFile(name, cb, attrib)` | creates an empty file. `ErcDupName` if the name exists |
| `CreateDir(name, cb)` | creates a directory |
| `DeleteDir(name, cb)` | removes an **empty** directory (`ErcDirNotEmpty` otherwise). MMURTL never finished this call; DOORS implements it |
| `OpenFile(...)` / `CloseFile(h)` | see above |
| `DeleteFile(h)` | deletes an open file (it must be open with `ModeModify`) and closes the handle |
| `RenameFile(old, cbOld, new, cbNew)` | renames or moves a file or directory within one volume (`ErcRenameDrv` across volumes, `ErcFileInUse` if it is open) |
| `ReadBytes(h, buf, n, &done)` | stream read at the LFA. Returns `ErcEOF` when fewer than `n` bytes were left |
| `WriteBytes(h, buf, n, &done)` | stream write at the LFA. Extends the file as needed |
| `ReadBlock(h, buf, n, lfa, &done)` | block read. `ErcBeyondEOF` if `lfa` is at or past the end |
| `WriteBlock(h, buf, n, lfa, &done)` | block write. Extends the file as needed (MMURTL lost the last partial block when copying; DOORS does not) |
| `GetFileSize(h, &size)` / `SetFileSize(h, size)` | `SetFileSize` truncates or extends (the new part reads as zeros) |
| `GetFileLFA(h, &lfa)` / `SetFileLFA(h, lfa)` | stream position. `lfa = 0xFFFFFFFFFFFFFFFF` means "end of file". Past the end gives `ErcBeyondEOF` |
| `GetDirSector(path, cb, buf, 512, n)` | sector `n` of a FAT directory, raw 32-byte entries, as in MMURTL. FAT only (`ErcNotSupported` on ext4) |
| `ReadDir(path, cb, index, &entry)` | DOORS: entry number `index` of a directory on any file system. `ErcNoMatch` after the last one |
| `GetSystemDisk(&d)` | MMURTL's system disk number: 2 (C:) if C: exists |

### ReadDir

```c
struct dir_entry {              /* include/doors/dirent.h */
	char name[256];         /* NUL-terminated, the long name if there is one */
	U64  size;
	U32  attr;              /* ATTR_DIR, ATTR_RO, ... */
	U32  mtime;             /* Unix time (seconds since 1970) */
};

struct dir_entry e;
for (U32 i = 0; ReadDir("C:\\", 3, i, &e) == ErcOk; i++)
	printf("%s %lu\n", e.name, e.size);
```

Neither file system lists `.` and `..`. FAT also skips the volume label and deleted entries.

### Attributes

`CreateFile`'s `attrib` and `dir_entry.attr` use the FAT bits: `ATTR_RO` 0x01, `ATTR_HIDDEN`
0x02, `ATTR_SYSTEM` 0x04, `ATTR_DIR` 0x10, `ATTR_ARCH` 0x20. On ext4 only `ATTR_DIR` is reported.

## FAT

`kernel/fat.c` supports FAT12, FAT16 and FAT32. The type is chosen by cluster count, as the
specification says.

- **Long names (VFAT):** read and created. A new long name also gets an 8.3 alias (`NAME~N`).
- Create, delete, rename, move (fixing `..` in moved directories), mkdir and rmdir.
- Every FAT copy is updated. On FAT32 the FSInfo free count is marked unknown, so other systems
  recount it.
- Timestamps come from the CMOS clock and are stored as local time, as FAT requires.
- After every `make test`, Linux's `fsck.fat` checks the volumes DOORS wrote.

## ext4

`kernel/ext4.c` reads and writes volumes made by Linux's `mkfs.ext4` with its default features:
`64bit`, `extents`, `flex_bg`, `metadata_csum`, `dir_index`, `huge_file`, `dir_nlink`,
`extra_isize`. Block sizes up to 4 KB are supported.

- **Checksums** (CRC32C) are checked on read and written on every change: superblock, group
  descriptors, bitmaps, inodes, extent blocks, directory blocks.
- **Files** use extent trees, with holes (sparse files) and unwritten extents. Sizes are 64-bit:
  a 5 GB sparse file is part of the tests.
- **Writes:** block and inode allocation, extending files, truncating, sparse extension,
  create/delete, rename/move (directories get their `..` and link counts fixed), mkdir/rmdir.
- **Times** are Unix time (UTC).
- After every `make test`, Linux's `e2fsck -fn` checks the volume. Its contents are checked with
  `debugfs`.

### ext4 limits (until milestone M8b)

| Situation | Result |
|---|---|
| **No journal.** DOORS writes directly. A power cut during a write can leave the volume needing `e2fsck` | — |
| The volume's journal needs recovery (Linux was not shut down cleanly) | mounted **read-only** |
| Unknown read-only features, or old block-mapped (ext2/3-style) volumes | mounted read-only |
| Unknown incompatible features (encryption, inline data, ...) | not mounted |
| Writing to an indexed (htree) directory, as Linux makes for large directories | `ErcNotSupported`. Small directories are plain lists and work |
| Writing a file whose extent tree is deeper than 1 level (very fragmented, written by Linux) | `ErcNotSupported`. Reading works at any depth |
| Reading a block-mapped (ext2/3-style) file | `ErcNotSupported` |

On a read-only volume, every call that would change something returns `ErcReadOnly`.
