/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* upcli: a client of the user-mode UPCASE service (upsvc), in another job. */

#include "doors.h"

static const char text[] = "doors from user mode";
static char out[sizeof(text)];

int main(void)
{
	U32 exch;
	U64 h, m[2];
	erc_t erc;
	int i;

	GetTSSExch(&exch);
	for (i = 0; i < 100; i++) {		/* wait until the service has registered */
		erc = Request("UPCASE  ", 1, exch, &h, 1, (U8 *)text, sizeof(text), (U8 *)out,
			      sizeof(out), 0, 0, 0);
		if (erc != ErcNoSuchSvc)
			break;
		Sleep(2);
	}
	if (erc || WaitMsg(exch, m) || m[0] != h || m[1] != ErcOk) {
		printf("upcli: request failed (erc %u)\n", erc);
		return 1;
	}
	printf("upcli: UPCASE replied '%s'\n", out);
	Request("UPCASE  ", 0, exch, &h, 0, 0, 0, 0, 0, 0, 0, 0);	/* shut the service down */
	WaitMsg(exch, m);
	return memcmp(out, "DOORS FROM USER MODE", sizeof(out)) ? 2 : 0;
}
