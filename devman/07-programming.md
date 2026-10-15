<!-- DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. -->
# 7. Writing programs

DOORS programs are static 64-bit ELF executables written in C against `port64/user/lib/doors.h`.
That header declares the MMURTL calls with their original names and parameter order. Programs run
in ring 3, each in its own job and address space.

## Your first program

1. Create `port64/user/progs/greet.c`:

   ```c
   /* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
   /* greet: says hello and prints its job number and command line. */
   #include "doors.h"

   int main(void)
   {
   	U32 job, n;
   	char cmd[80];

   	GetJobNum(&job);
   	GetCmdLine(cmd, &n);
   	cmd[n < sizeof(cmd) ? n : sizeof(cmd) - 1] = 0;
   	printf("greet: hello from job %u, command line '%s'\n", job, cmd);
   	return 0;		/* becomes ExitJob(0) */
   }
   ```

2. Build: `make` (in `port64/`). Every `user/progs/NAME.c` is built to `build/user/NAME.elf` and
   put on the boot image as module `NAME`. No Makefile change is needed.
3. Run it from the monitor: `run greet`. To start it at boot, add a line `greet` to
   `port64/user/INITIAL.JOB`.
4. To run it from a disk, copy `build/user/greet.elf` onto the disk image (for example with
   `mcopy` onto C:) and type `run C:\GREET.ELF`.

The monitor prints `Job number N terminated. Error: 0` when the program ends.

## How a program starts and ends

- Programs are linked at `0x40000000` (`user/user.ld`). Code is mapped read-only and executable,
  constant data read-only, and data/bss read-write and non-executable.
- Execution starts at `_start` (`user/lib/crt0.asm`), which calls `main()`.
- When `main` returns, its return value is passed to `ExitJob`. Calling `ExitJob(erc)` directly works
  too.
- The stack is 64 KB, with a guard page below it. Overflowing it is a page fault, and the job ends
  with error 1014.
- More memory: `AllocPage(nPages, &p)` gives 4 KB pages, `DeAllocPage(p, nPages)` returns them.
  There is no `malloc` yet.

## The user library

`user/lib/libdoors.c` contains:

- a stub for every system call DOORS supports ([chapter 8](08-system-calls.md));
- MMURTL's memory helpers, which run in the library without entering the kernel: `CopyData`,
  `CopyDataR`, `FillData`, `Compare` and `CompareNCS` (`Compare` returns the index of the first
  difference, or the size if the buffers are equal);
- a small C library: `memcpy`, `memset`, `memcmp`, `strlen`, and `printf`.

`printf` prints through `TTYOut`. It understands `%c %s %d %u %x %p %%`, the `l`/`ll` length
modifiers (`%lu`, `%lx`) and a field width with optional zero padding (`%5u`, `%08lx`).

Floating point (SSE) may be used: DOORS saves each task's FPU/SSE state.

### Calling convention for system calls

The library builds an array of `U64` arguments in MMURTL order and executes `SYSCALL` with
`RAX` = call number and `RDI` = address of the array. The result returns in `RAX`. To call a number
directly: `doors_syscall(num, args)`.

## Strings and lengths

As in MMURTL, most calls take a pointer **and a length** instead of a NUL-terminated string:

```c
OpenFile("C:\\HELLO.TXT", 12, ModeRead, 1, &h);
```

In C source a backslash in a path is written `\\`. Service names are exactly 8 characters, padded
with spaces: `"FILESYS "`.

## Error handling

Every call returns an `erc_t`. `ErcOk` (0) means success. Check it:

```c
erc_t erc = OpenFile(name, len, ModeRead, 1, &h);
if (erc) {
	printf("cannot open %s: error %u\n", name, erc);
	ExitJob(erc);
}
```

The codes are listed in [chapter 9](09-error-codes.md).

## Bad pointers

The kernel checks every pointer a program passes. A pointer into kernel memory, an unmapped
address, or (for output) read-only memory makes the call return an error, usually `ErcBadLinAdd`
(or `ErcNullPtr` for a null pointer). The call is not performed, and the program keeps running. Touching a bad pointer **directly** in the program is
a CPU fault, and the job ends with error `1000 + vector`. `user/progs/badptr.c` demonstrates both.

## Several tasks in one program

```c
static U8 stack2[16384] __attribute__((aligned(16)));

static void worker(void)
{
	for (;;) {
		printf("worker\n");
		Sleep(100);
	}
}

/* in main: */
SpawnTask(worker, 25, 0, stack2 + sizeof(stack2), 0);
```

`SpawnTask(entry, priority, fDebug, stackTop, fOSCode)` starts another task in the same job. The
`stackTop` is the **end** of a memory block you provide. `fOSCode` must be 0 in user programs.
All tasks of a job end when the job ends.

## Writing a service

A service registers a name and serves requests in a loop. This one upper-cases text. It is
`user/progs/upsvc.c` with full error handling added:

```c
#include "doors.h"

int main(void)
{
	U32 exch;
	U64 m[2];

	if (AllocExch(&exch) || RegisterSvc("UPCASE  ", exch))
		return 1;
	for (;;) {
		erc_t erc = WaitMsg(exch, m);
		const struct RqBlk *r = (const struct RqBlk *)(uintptr_t)m[0];
		U64 i;

		if (erc) {			/* the request arrived, but its buffers could not be mapped */
			if (erc == ErcBadLinAdd || erc == ErcNoMem || erc == ErcBadJobNum)
				Respond(m[0], erc);
			continue;
		}
		if (r->ServiceCode == 0) {	/* shut down */
			UnRegisterSvc("UPCASE  ");
			Respond(m[0], ErcOk);
			return 0;
		}
		for (i = 0; i < r->cbData1 && i < r->cbData2; i++) {
			U8 c = r->pData1[i];
			r->pData2[i] = (c >= 'a' && c <= 'z') ? (U8)(c - 32) : c;
		}
		Respond(m[0], ErcOk);
	}
}
```

Points to note:

- `m[0]` is the request handle. In a user service it points into a read-only window, so the
  service reads the `struct RqBlk` (`include/doors/rqblk.h`) but cannot change it.
- `r->pData1` and `r->pData2` are already mapped into the service's address space. Here the
  client sent `pData1` (`npSend` = 1) and the service writes `pData2`.
- `r->RqOwnerJob` is the client's job number. `r->dData0..2` carry three small values.
- Answer every request with `Respond`. Otherwise the client waits forever.

The client side is in `user/progs/upcli.c` and in [chapter 5](05-messaging.md#example-a-client).

## What is not available yet

Per-job screens (`SetVidOwner`, `PutVidChars`, ...), the keyboard service (`ReadKbd`), `Chain`,
`SetExitJob`, `EditLine` and the other calls marked "not yet" in [chapter 8](08-system-calls.md)
return `ErcBadCallGate`. Programs today write text with `TTYOut`/`printf` to the shared console.
