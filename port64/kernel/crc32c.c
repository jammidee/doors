/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* CRC32C (Castagnoli, reflected polynomial 0x82F63B78), as ext4 metadata checksums use it.

   crc32c() is the raw register update with no initial or final inversion, which is what the
   Linux kernel's ext4_chksum() and e2fsprogs' ext2fs_crc32c_le() compute. The textbook CRC32C of
   a buffer is ~crc32c(~0, buf, len); crc32c_selftest() checks that against the standard vector
   ("123456789" -> 0xE3069283). */

#include <doors/fs.h>

static U32 table[256];
static bool ready;

static void init(void)
{
	U32 i, j, c;

	for (i = 0; i < 256; i++) {
		c = i;
		for (j = 0; j < 8; j++)
			c = (c >> 1) ^ ((c & 1) ? 0x82F63B78U : 0);
		table[i] = c;
	}
	ready = true;
}

U32 crc32c(U32 crc, const void *buf, U64 len)
{
	const U8 *p = buf;

	if (!ready)
		init();
	while (len--)
		crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return crc;
}

bool crc32c_selftest(void)
{
	return ~crc32c(~0U, "123456789", 9) == 0xE3069283U;
}
