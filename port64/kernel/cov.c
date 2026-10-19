/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Test coverage (ROADMAP 2.6, `make coverage`). In a coverage build (COV=1: the kernel compiled
   with -fsanitize-coverage=trace-pc and DOORS_COVERAGE) GCC calls __sanitizer_cov_trace_pc at the
   start of every basic block; it marks the block's return address in a bitmap over .text. At the
   end of the self-tests cov_dump() prints the bitmap's non-empty parts on the console, and
   tools/coverage.py turns them into source lines. This file itself is built without the
   instrumentation (it would call itself). In a normal build it is empty. */

#include <doors/console.h>
#include <doors/types.h>

#ifdef DOORS_COVERAGE

#define COV_TEXT_MAX (2U << 20)			/* .text may be up to 2 MB */

extern U8 _text_start[], _text_end[];
static U8 cov_bits[COV_TEXT_MAX / 8];

void __sanitizer_cov_trace_pc(void);
void __sanitizer_cov_trace_pc(void)
{
	U64 off = (U64)(uintptr_t)__builtin_return_address(0) - (U64)(uintptr_t)_text_start;

	if (off < COV_TEXT_MAX)
		cov_bits[off >> 3] |= (U8)(1 << (off & 7));
}

/* "COV text <start>" then "COV <offset> <hex of 32 bitmap bytes>" for every non-empty chunk */
void cov_dump(void)
{
	static const char hex[] = "0123456789abcdef";
	U64 n = ((U64)(uintptr_t)_text_end - (U64)(uintptr_t)_text_start + 7) / 8, i, k;
	char line[80];

	kprintf("COV text %lx\n", (U64)(uintptr_t)_text_start);
	for (i = 0; i < n && i < sizeof(cov_bits); i += 32) {
		bool any = false;

		for (k = 0; k < 32; k++)
			any |= cov_bits[i + k] != 0;
		if (!any)
			continue;
		for (k = 0; k < 32; k++) {
			line[2 * k] = hex[cov_bits[i + k] >> 4];
			line[2 * k + 1] = hex[cov_bits[i + k] & 15];
		}
		line[64] = 0;
		kprintf("COV %lx %s\n", i, line);
	}
	kprintf("COV end\n");
}

#else

void cov_dump(void)
{
}

#endif
