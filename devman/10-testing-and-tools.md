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
disk check prints `CHECK_DISK PASS`. Status 35, a `DOORS PANIC: ...` line, or a timeout (120 s)
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

## Tools

| Tool | Purpose |
|---|---|
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
`build/gen/doors/version.h`. The kernel prints both at boot (`Version 0.9.0, build v0.9.0`), the
GRUB menu shows the version, and the monitor's `ver` command repeats it. A build from a working tree
with uncommitted changes is marked `-dirty`.

Versions are `0.M.P` until the port matches MMURTL (M9 will be 1.0.0): `M` is the milestone and
`P` counts the changes within it. Every release commit updates `port64/VERSION` and `CHANGELOG.md`
and is tagged `vX.Y.Z` in git.
