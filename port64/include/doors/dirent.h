/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Shared by the kernel and user programs: file attributes and the ReadDir entry (DOORS call 110,
   works on every file system, unlike MMURTL's FAT-only GetDirSector). */
#ifndef DOORS_DIRENT_H
#define DOORS_DIRENT_H

#include <doors/types.h>

/* FAT attributes (also CreateFile's Attrib); ext4 maps directories and read-only files onto them */
#define ATTR_RO     0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYSTEM 0x04
#define ATTR_VOLUME 0x08
#define ATTR_DIR    0x10
#define ATTR_ARCH   0x20
#define ATTR_LFN    0x0F

struct dir_entry {
	char name[256];			/* NUL-terminated, long name if there is one */
	U64 size;
	U32 attr;			/* ATTR_DIR, ATTR_RO, ... */
	U32 mtime;			/* Unix time */
};

#endif
