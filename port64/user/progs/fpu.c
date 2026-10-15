/* fpu: floating point / SSE in user mode. Two copies run at once; each keeps doubles in SSE
   registers across Sleep calls and preemption. If task switches didn't save and restore the
   x87/SSE state, the results would mix. */

#include "doors.h"

static double run(double seed, int n, bool sleepy)
{
	double x = seed, acc = 0.0;
	int i;

	for (i = 1; i <= n; i++) {
		x = x * 1.000001 + 0.5 / i;
		acc += x / (1.0 + (double)(i & 7));
		if (sleepy && (i % 2000) == 0)
			Sleep(1);		/* let the other copy run in the middle of it */
	}
	return acc;
}

int main(void)
{
	U32 job;
	double ref, got;
	U64 bits_ref, bits_got;

	GetJobNum(&job);
	ref = run(job * 1.25, 40000, false);	/* reference, mostly without switches */
	got = run(job * 1.25, 40000, true);	/* same work, interleaved with the other job */
	memcpy(&bits_ref, &ref, 8);
	memcpy(&bits_got, &got, 8);
	if (bits_ref != bits_got) {
		printf("fpu: job %u MISMATCH %lx != %lx\n", job, bits_got, bits_ref);
		return 1;
	}
	printf("fpu: job %u ok (result %d.%03d bit-identical across task switches)\n", job,
	       (int)got, (int)((got - (int)got) * 1000));
	return 0;
}
