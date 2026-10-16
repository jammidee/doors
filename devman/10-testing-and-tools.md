<!-- DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. -->
# 10. Testing and tools

## `make test`

`make test` (in `port64/`) is the full check of the system. Run it after every change. It:

1. builds `build/doors-test.iso`, the same system with the kernel command line `test`;
2. builds a fresh test disk `build/disk-test.img` with `tools/mkdisk.py`;
3. boots it headless with `tools/qemu_test.py`. The kernel runs the self-tests of every milestone
   (`kernel/selftest.c`), prints a line for each part, and ends QEMU through the `isa-debug-exit`
   device;
4. checks the disk the kernel wrote with Linux tools (`tools/check_disk.py`).

It passes when QEMU exits with status 33, every expected line appeared in the serial log, and the
disk check prints `CHECK_DISK PASS`. Status 35, a `DOORS PANIC: ...` line, or a timeout (240 s; a run takes about 75 s)
is a failure. The serial log is kept in `build/serial.log`.

### What the self-tests cover

| Milestone | Lines | Checks |
|---|---|---|
| M1 | `DOORS: M1 OK` | long mode, banner |
| M2 | `M2: timer ok`, `breakpoint ok`, `keyboard ok`, `exception dump ok` | interrupts, timer rate, keyboard input (typed in by the test runner), exception reports |
| M3 | `pmm ok`, `paging ok`, memory calls, `address spaces ok`, stack guard | physical and virtual memory |
| M4 | `priority ok`, `sleep ok`, `round-robin ok`, `context switch ok`, task calls | the scheduler |
| M5 | exchanges, `WaitMsg/SendMsg`, ping-pong, alarms, keyboard messages, `Request/Respond`, cross-job requests, `no leaks` | messaging |
| M6 | the `hello`, `badptr`, `fpu`, `upsvc`/`upcli` programs, `KillJob`, `no leaks` | user mode and jobs |
| M7 | `disk ok`, `FAT read ok`, `FAT write ok`, loading from a file, `fsuser` in ring 3 | disks and FAT |
| M8 | `ext4 read ok`, `ext4 write ok`, `ext4 stress ok (seed ...)`, loading from ext4 | ext4 |
| M9 | `vidtest: all video and keyboard calls ok`, `M9: video and keyboard ok` | virtual screens, `ReadKbd` with keys typed by the runner (`{f1}`, `{up}`, `{backspace}`), `EditLine`, Ctrl-Alt-PgDn/Del |
| M9 | `jobtest: job calls, Chain, exit job and C library ok`, `M9: job calls ok` | JCB strings, CMOS clock, the C library, `Chain` and the exit run file (three programs in one job) |
| M9 | `hello: command line 'hello'`, `M9: CLI ok` | the CLI from `C:\DOORS\CLI.ELF` with typed commands: `md`, `copy`, an external command found in `C:\DOORS\` and run with `Chain`, `edit` (create a file with a long name, type, Alt-S, Alt-Q), `dir`, `exit`; `check_disk.py` checks both files |
| M9 | `NUMBERS Service gave out number: 1.`, `M9: samples ok` | the NUMBERS service (waits for a key through the KEYBOARD service) with two `testsvc` clients; BusyLoop killed with Ctrl-Alt-Del; no pages leaked |

The expected lines are listed in `TEST_ARGS` in the Makefile (`--expect`). Lines the test runner
answers with keystrokes are `--send` entries (`"M2: type doors=doors\n"` means: when `M2: type
doors` appears, type `doors` and Enter).

### The ext4 stress test

`test_ext4_stress` performs 400 random operations on 16 files in `F:\STRESS`: write, truncate,
sparse extend, rename and move between directories, delete, overwrite, and creating and removing
a directory. After each operation it checks the result against a model of what every file must
contain. The random seed changes on every run and is printed (`seed 42043961256d00a9`). Run
`make test` several times to cover more cases. If a run fails, the seed is in the panic message.

## Host-side disk checks (`tools/check_disk.py`)

After the kernel has written to the test disk, Linux's own tools check it:

- `fsck.fat -n` on C:, D: and E: must report no errors;
- `e2fsck -fn` on F: must report a clean volume;
- the files the tests wrote are read back with `mtools` (FAT) and `debugfs` (ext4) and compared
  byte for byte, including long names, the 5 GB sparse file, moved directories and deleted files.

Because the disk is made by Linux's `mkfs` tools and checked by Linux's `fsck` tools, DOORS's
file systems are tested against independent implementations.

## `make smoke`: the smoke test

`make smoke` (`port64/tools/smoke64.py`) runs `docs/SMOKE_TEST.md` the way a person would. It
boots the normal ISO on a fresh disk in a headless QEMU, types into the monitor and the CLIs through
the QEMU monitor, and reads the text screen. Rows: boot and status line, F1 LdCLI, every CLI file
command (`dir`, `type`, `md`, `copy`, `ren`, `del`, `rd`, `path`, `dump`), `help`, the editor (also with
a long path), the NUMBERS service across two CLIs, the monitor's F2/F3, Ctrl-Alt-PgDn, BusyLoop +
Ctrl-Alt-Del, `monitor` and `exit`. It ends with `PASS n  FAIL n  DEFERRED n` and exits non-zero on a
failure. `--only F5,P2` runs just those rows (plus boot and LdCLI); `--keep` leaves the VM running.
It takes about 1.5 minutes. 1.0.0: 24 PASS, 1 DEFERRED (the debugger, M10).

## `make powercut`: power cuts during ext4 writes

`make powercut` (`port64/tools/powercut.py`) tests the ext4 journal. Each run builds a fresh disk
and boots DOORS with the kernel command line `powercut`. DOORS then writes, rewrites, renames
and deletes files in `F:\PC` forever, printing `PC: op N done` after each operation
(`kernel/powercut.c`). The script kills QEMU (SIGKILL) at a random point. Run 0 boots
`powercut stop=60` instead, which halts inside the 60th journal commit, after the transaction is in
the journal and before its blocks are in place, so a replay is certain. After each cut:

1. **Linux** replays the journal on a copy (`e2fsck -fy`). `e2fsck -fn` must then be clean, and
   `debugfs` must show every file exactly as the operation model says after the last `done`
   operation. Only the files of the operation in progress may be either way.
2. **DOORS** boots with `pccheck` on the same disk. The mount replays the journal, DOORS reads every
   file, and it exits cleanly. Then `e2fsck -fn` must be clean **without any repair**, and the model
   must hold again.

`--runs N` (default 5) and `--seed S` choose the runs. It ends with `POWERCUT PASS`.

## Tools

| Tool | Purpose |
|---|---|
| `port64/tools/powercut.py` | the power-cut test (`make powercut`) |
| `port64/tools/smoke64.py` | the 64-bit smoke test (`make smoke`) |
| `port64/tools/qemu_test.py` | headless test runner: `--iso`, `--disk`, `--log`, `--expect TEXT` (repeatable), `--send "TRIGGER=TEXT"` (`\n` = Enter, `{name}` = a raw QEMU key such as `{f1}`), `--timeout` |
| `port64/tools/mkdisk.py OUT.img [--hello ELF]` | builds the 4-partition test disk |
| `port64/tools/check_disk.py DISK.img` | the host-side disk checks |
| `tools/gen_syscalls.sh` | regenerates `syscalls.def` from MMURTL's `MPUBLICS.ASM` plus the DOORS calls |
| `tools/boot32/qscreen.py` | reads the QEMU text screen and types keys through a QEMU monitor socket: `--sock PATH`, `--keys "d i r spc ret"`, `--cmd quit` |
| `tools/boot32/run.sh`, `smoke.py` | boot and smoke-test the original 32-bit MMURTL |

## Debugging

- **Serial log:** every kernel message goes to COM1 as well as the screen. `make run` shows it in
  the terminal; `make console` writes it to `build/serial-run.log`.
- **Kernel panics** print the reason. CPU exceptions in the kernel print a full register dump.
- **gdb:** `make debug`, then in another terminal:
  `gdb build/kernel.elf -ex 'target remote :1234'`. The kernel is built with `-g`.
- **QEMU monitor:** add `QEMU_EXTRA="-monitor unix:/tmp/doors.mon,server,nowait"` to look at
  registers, memory and the screen from outside (`info registers`, `x/16gx ADDR`).
- **Leaks:** the M5/M6 tests compare the free counts of pages, exchanges, link blocks and request
  blocks before and after running jobs. The monitor's `tasks` command shows the same counts.

## Versions and the changelog

The version is in `port64/VERSION`. The build turns it and the git commit into
`build/gen/doors/version.h`. The kernel prints both at boot (`Version 1.0.0, build v1.0.0`), the
GRUB menu shows the version, and the monitor's `ver` command repeats it. A build from a working tree
with uncommitted changes is marked `-dirty`.

Versions were `0.M.P` while the port was being built (`M` = milestone). 1.0.0 is the release that passes
the smoke test. From there on: MINOR for new features, PATCH for fixes. Every release commit updates
`port64/VERSION` and `CHANGELOG.md` and is tagged `vX.Y.Z` in git.
