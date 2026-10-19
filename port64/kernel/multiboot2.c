/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Multiboot2 boot information parsing: command line, loader name and memory map. */

#include <doors/console.h>
#include <doors/multiboot2.h>
#include <doors/string.h>

static void copy(char *dst, U32 size, const char *src)
{
	U32 n = 0;

	while (n < size - 1 && src[n]) {
		dst[n] = src[n];
		n++;
	}
	dst[n] = 0;
}

struct boot_info boot_info;

const struct boot_module *boot_module(const char *name, U32 len)
{
	U32 i;

	for (i = 0; i < boot_info.nmods; i++)
		if (strlen(boot_info.mods[i].name) == len && !memcmp(boot_info.mods[i].name, name, len))
			return &boot_info.mods[i];
	return NULL;
}

static const char *mmap_type(U32 t)
{
	switch (t) {
	case 1: return "available";
	case 2: return "reserved";
	case 3: return "ACPI reclaimable";
	case 4: return "ACPI NVS";
	case 5: return "bad";
	default: return "other";
	}
}

void mb2_parse(U64 mbi_phys, struct boot_info *bi, bool print)
{
	const struct mb2_info *mbi = PHYS_TO_VIRT(mbi_phys);
	const U8 *p = (const U8 *)mbi + 8;
	const U8 *end = (const U8 *)mbi + mbi->total_size;

	memset(bi, 0, sizeof(*bi));
	bi->loader[0] = '?';
	bi->mbi_size = mbi->total_size;

	while (p < end) {
		const struct mb2_tag *tag = (const struct mb2_tag *)p;

		if (tag->type == MB2_TAG_END)
			break;

		switch (tag->type) {
		case MB2_TAG_CMDLINE:
			copy(bi->cmdline, sizeof(bi->cmdline), ((const struct mb2_tag_string *)tag)->string);
			break;
		case MB2_TAG_LOADER_NAME:
			copy(bi->loader, sizeof(bi->loader), ((const struct mb2_tag_string *)tag)->string);
			break;
		case MB2_TAG_MODULE: {
			const struct mb2_tag_module *m = (const struct mb2_tag_module *)tag;
			struct boot_module *bm;
			const char *s = m->string;
			U32 n = 0;

			if (bi->nmods >= 16)
				break;
			bm = &bi->mods[bi->nmods++];
			bm->start = m->mod_start;
			bm->end = m->mod_end;
			while (*s == ' ')
				s++;
			while (s[n] && s[n] != ' ' && n < sizeof(bm->name) - 1) {
				bm->name[n] = s[n];
				n++;
			}
			s += n;
			while (*s == ' ')
				s++;
			copy(bm->args, sizeof(bm->args), s);
			break;
		}
		case MB2_TAG_MMAP: {
			const struct mb2_tag_mmap *m = (const struct mb2_tag_mmap *)tag;
			const U8 *e = (const U8 *)m->entries;

			if (print)
				kprintf("Memory map:\n");
			for (; e < p + tag->size; e += m->entry_size) {
				const struct mb2_mmap_entry *me = (const struct mb2_mmap_entry *)e;

				if (me->type == MB2_MEM_AVAILABLE)
					bi->mem_usable += me->length;
				bi->mmap_count++;
				if (print)
					kprintf("  %016lx - %016lx  %s\n", me->base,
						me->base + me->length - 1, mmap_type(me->type));
			}
			break;
		}
		}
		p += (tag->size + 7) & ~7u;	/* tags are 8-byte aligned */
	}
}

bool cmdline_has(const char *cmdline, const char *word)
{
	const char *p = cmdline;

	while (*p) {
		const char *w = word, *q = p;

		while (*q == ' ')
			q++;
		p = q;
		while (*w && *q == *w) {
			q++;
			w++;
		}
		if (!*w && (*q == ' ' || *q == '\0'))
			return true;
		while (*p && *p != ' ')
			p++;
	}
	return false;
}
