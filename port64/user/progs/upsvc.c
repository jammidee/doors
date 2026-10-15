/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* upsvc: a system service written as an ordinary user program (like MSAMPLES/SERVICE).
   Registers "UPCASE  ", reads each request block through the read-only window the kernel gives
   user services, upper-cases pData1 into pData2 (both aliased into this job by the kernel), and
   responds. Service code 0 shuts it down. */

#include "doors.h"

int main(void)
{
	U32 exch;
	U64 m[2];
	int served = 0;

	if (AllocExch(&exch) || RegisterSvc("UPCASE  ", exch)) {
		printf("upsvc: cannot register\n");
		return 1;
	}
	printf("upsvc: UPCASE service ready on exchange %u\n", exch);
	for (;;) {
		const struct RqBlk *r;
		U64 i;

		if (WaitMsg(exch, m))
			continue;
		r = (const struct RqBlk *)(uintptr_t)m[0];
		if (r->ServiceCode == 0) {
			UnRegisterSvc("UPCASE  ");
			Respond(m[0], ErcOk);
			printf("upsvc: served %d requests, shutting down\n", served);
			return 0;
		}
		for (i = 0; i < r->cbData1 && i < r->cbData2; i++) {
			U8 c = r->pData1[i];

			r->pData2[i] = (c >= 'a' && c <= 'z') ? (U8)(c - 32) : c;
		}
		served++;
		Respond(m[0], ErcOk);
	}
}
