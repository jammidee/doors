/* Multiboot2 boot information (only the tags DOORS uses). */
#ifndef DOORS_MULTIBOOT2_H
#define DOORS_MULTIBOOT2_H

#include <doors/types.h>

#define MB2_BOOTLOADER_MAGIC 0x36D76289

#define MB2_TAG_END          0
#define MB2_TAG_CMDLINE      1
#define MB2_TAG_LOADER_NAME  2
#define MB2_TAG_BASIC_MEMINFO 4
#define MB2_TAG_MMAP         6

#define MB2_MEM_AVAILABLE    1

struct mb2_info {
	U32 total_size;
	U32 reserved;
} PACKED;

struct mb2_tag {
	U32 type;
	U32 size;
} PACKED;

struct mb2_tag_string {
	U32 type;
	U32 size;
	char string[];
} PACKED;

struct mb2_tag_meminfo {
	U32 type;
	U32 size;
	U32 mem_lower;	/* KB below 1 MB */
	U32 mem_upper;	/* KB above 1 MB */
} PACKED;

struct mb2_mmap_entry {
	U64 base;
	U64 length;
	U32 type;
	U32 reserved;
} PACKED;

struct mb2_tag_mmap {
	U32 type;
	U32 size;
	U32 entry_size;
	U32 entry_version;
	struct mb2_mmap_entry entries[];
} PACKED;

/* What the kernel keeps from the boot information. */
/* Copies, so they stay valid after the boot page tables (and their view of the MBI) are gone. */
struct boot_info {
	char cmdline[128];
	char loader[64];
	U64 mem_usable;		/* bytes of available RAM (sum of mmap type 1) */
	U32 mmap_count;
	U32 mbi_size;		/* total size of the boot information (kept reserved by the pmm) */
};

void mb2_parse(U64 mbi_phys, struct boot_info *bi, bool print);
bool cmdline_has(const char *cmdline, const char *word);

#endif
