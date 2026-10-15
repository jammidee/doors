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
- MMURTL's helpers, which run in the library without entering the kernel: `CopyData`,
  `CopyDataR`, `FillData`, `Compare` and `CompareNCS`, and `EditLine`. As in MMURTL, `Compare`
  returns **-1** when the bytes are equal and otherwise the offset of the first difference.

`user/lib/libc.c` is a small C library with the headers `<stdio.h>`, `<string.h>`, `<ctype.h>`
and `<stdlib.h>` (in `user/lib/libc/`), covering what CM32's library offered:

| Header | Functions |
|---|---|
| `stdio.h` | `printf`, `sprintf`, `snprintf`, `fprintf`, `vprintf`, `vsprintf`, `vsnprintf`; `fopen`, `fclose`, `fgetc`, `fgets`, `fputc`, `fputs`, `fread`, `fwrite`, `ftell`, `fseek`, `rewind`, `remove`, `rename`, `getc`, `putc`, `putchar`, `puts`; `stdin` (keyboard), `stdout`/`stderr` (the job's screen) |
| `string.h` | `mem*`, `strlen`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`, `strncmp`, `strchr`, `strrchr`, `strstr` |
| `ctype.h` | `isspace`, `isdigit`, `isalpha`, `isalnum`, `isupper`, `islower`, `isxdigit`, `ispunct`, `isgraph`, `isprint`, `iscntrl`, `toupper`, `tolower` (ASCII) |
| `stdlib.h` | `exit` (= `ExitJob`), `atoi`, `atol`, `strtol`, `strtoul`, `abs`, `labs` |

The formatter understands `%d %i %u %x %X %o %c %s %p %%`, the flags `-` and `0`, a width and a
precision (also `*`), and `l`/`ll`/`z`/`h`. `fopen` opens MMURTL stream files. `"r"` reads,
`"w"` creates or empties, `"a"` creates or appends, and a `+` adds write access. Reads are buffered
512 bytes at a time, and at most 16 files are open through `fopen` at once. There is no `malloc` yet.

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

The calls marked "not yet" in [chapter 8](08-system-calls.md) return `ErcBadCallGate`.

## Running other programs: Chain and the exit run file

A job can run several programs one after another, as MMURTL's CLI does:

```c
SetExitJob("C:\\DOORS\\CLI.ELF", 16);   /* come back to me when the next program exits */
SetCmdLine("C:\\README.TXT", 13);       /* the next program reads this with GetCmdLine */
Chain("C:\\BIN\\VIEW.ELF", 15, 0);      /* replace me with VIEW in this job */
/* Chain returns only if VIEW.ELF can't be loaded */
```

`Chain` loads the new program into the **same job**. The job keeps its number, screen, keyboard
buffer, command line, path, user name and exit run file. Its old tasks, exchanges, services, open
files and memory are released, as when a job ends. When the new program calls `ExitJob` (or returns
from `main`), DOORS loads the exit run file into the job again. With no exit run file set, the job
ends. `KillJob`, Ctrl-Alt-Del and a CPU fault always end the job; they never load the exit run file.

## Screen and keyboard

Each program has its own 80×25 screen. It draws there even when it isn't shown. To be seen and
to get keys, a program takes the screen, like MMURTL's CLI does:

```c
U32 job, key;

GetJobNum(&job);
SetVidOwner(job);                       /* show my screen; keys now come to me */
ClrScr();
PutVidChars(30, 0, "My program", 10, BRITEWHITE | BGBLUE);
SetXY(0, 2);
printf("Press a key...\n");
ReadKbd(&key, 1);                       /* wait for one */
if ((key & 0xFF) == 0x0F)               /* F1 */
	printf("F1, with%s Shift\n", (key & KBD_SHIFT) ? "" : "out");
```

When a program ends, its screen disappears and the monitor (job 1) is shown again if the program was
on screen. Everything written with `TTYOut`/`printf` is also copied to the serial log.
