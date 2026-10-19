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
disk check prints `CHECK_DISK PASS`. Status 35, a `DOORS PANIC: ...` line, or a timeout (240 s; a run takes about 2 minutes)
is a failure. The serial log is kept in `build/serial.log`. `QEMU_TEST=script` makes it run another
QEMU, for example a script that adds `-s` so `gdb` can attach to a hung test.

**`make fulltest`** is `make test` with the kernel command line `test full`. After the normal
tests it fills FAT32 C:, FAT16 D:, FAT12 E:, ext4 G: (1K blocks) and ext4 I: (4K blocks, a small
volume on the second disk) to 100 % with 1 MB files, checks what a full disk does, and frees
everything again (`1.7: full volumes ok`). Then the same disk checks run. It takes about 5
minutes, most of it ATA PIO, so it is a separate target (CI: ROADMAP 2.1). The edge cases,
0-byte files, the longest names, a full root directory, no inodes left (I:) and the read-only H:,
run in every `make test` (`1.7: edge cases ok`).

(KVM doesn't help here. WSL 2 offers `/dev/kvm`, but there it is nested inside Hyper-V, and every
port access of ATA PIO costs more than under plain emulation: a try with `-accel kvm` ran slower.)

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
| M8b | `ext4 on F: a block logged in transaction 2 has a bad checksum: the log ends there`, `ext4 on F: journal replayed: 1 transactions, 1 blocks`, `M8b: htree directory writes ok`, `M8b: deep extent trees ok` | F: has a csum_v3 journal holding two Linux transactions (`debugfs jo -c`): the first is replayed, the second's logged block was damaged by mkdisk and must not be (`check_disk.py` checks that CSUMBAD.TXT is unchanged); on the second test disk (GPT, G:, ext4 with 1K blocks): 450 creates, 100 deletes and 20 moves in Linux-indexed directories, with leaf and node splits and a root growing to two levels; a file written as 400 one-block fragments, cut to 200 and grown to 480 (a depth-2 extent tree), and a second one deleted. `check_disk.py` runs `e2fsck -fn` on G:, compares every name and checks FRAG2.BIN's tree |
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

- `fsck.fat -n` on C: and D: must report no errors;
- E: and H: start out dirty and damaged (mkdisk's `damage_fat12`). E: has a chain longer than its
  file, a chain too short, a lost cluster and FAT copies that differ: harmless, so it stays
  writable. H:, a FAT12 partition on the second (GPT) disk, has a chain reaching a free cluster
  and two files sharing clusters, so DOORS must mount it read-only. On both, `fsck.fat -n` must
  find exactly that damage, and the serial log (`--log`) must show that DOORS's mount check
  reported the same;
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
1.0.0: 24 PASS, 1 DEFERRED (the debugger, M10).

`make smoke` runs it twice. The second run, `smoke64.py --ext4`, boots from an ext4 system disk
(`mkdisk.py --system ext4`: GRUB on the disk, no ISO) and runs the same rows on it. Two of them
differ there on purpose: B4 expects C: = ext4 and D: = FAT16, and F9 expects case-sensitive names.
A last row, B8, checks the disk afterwards: `e2fsck -fn` must find C: clean with the files the rows
wrote, and the disk must boot again. Both runs together take about 3 minutes (1.7.0: 24 PASS and 25
PASS, 1 DEFERRED each).

## `make powercut`: power cuts during ext4 and FAT writes

`make powercut` (`port64/tools/powercut.py`) tests the ext4 journal. Each run builds a fresh disk
and boots DOORS with the kernel command line `powercut`. DOORS then writes, rewrites, renames
and deletes files in `F:\PC` forever, printing `PC: op N done` after each operation
(`kernel/powercut.c`). The script kills QEMU (SIGKILL) at a random point. Run 0 boots
`powercut stop=60` instead, which halts inside the 60th journal commit, after the transaction is in
the journal and before its blocks are in place, so a replay is certain. Even runs give F: a
csum_v3 journal and odd runs a plain one (`mkdisk.py --plain-journal`), so Linux's replay checks
every checksum DOORS wrote. After each cut:

1. **Linux** replays the journal on a copy (`e2fsck -fy`). `e2fsck -fn` must then be clean, and
   `debugfs` must show every file exactly as the operation model says after the last `done`
   operation. Only the files of the operation in progress may be either way.
2. **DOORS** boots with `pccheck` on the same disk. The mount replays the journal, DOORS reads every
   file, and it exits cleanly. Then `e2fsck -fn` must be clean **without any repair**, and the model
   must hold again.

Then come the **FAT runs** (`--fat-runs N`, default 5). DOORS boots with `powercut vol=C`, writes
in `C:\PC` and is cut at a random point. FAT has no journal, so the cut may leave damage:

1. **Linux:** `fsck.fat -n` on a copy must see the dirty flag. Its findings are read into a set,
   and an unknown line fails the run. mtools must show every finished operation, before any repair.
2. **DOORS** boots with `pccheck vol=C`. Its mount check must report exactly the findings of
   `fsck.fat` (the same files, problems and lost-cluster count). Only the files of the operation
   in progress may be unreadable.

`--runs N` (default 5), `--fat-runs N` and `--seed S` choose the runs. It ends with
`POWERCUT PASS`, in about 2 minutes.

## `make fuzz`: damaged disks

`make fuzz` checks that a damaged disk can't crash DOORS. The kernel's own volume, FAT, ext4,
CRC32C and block-cache code (`kernel/vol.c`, `fat.c`, `ext4.c`, `crc32c.c`, `bcache.c`) is compiled
unchanged into a Linux program, `port64/tests/fsfuzz/fsfuzz.c`, over a disk image in memory. That
makes it thousands of times faster than booting QEMU for every image, and memory errors that the
kernel would not notice are caught too.

- **The disks** (`tools/fuzzdisk.py`, in `build/fuzz/`):
  - `fat.img`: FAT12, FAT16 and FAT32, with long names, nested directories, a directory of 150
    files and a fragmented file;
  - `ext4.img`: ext4 with 4K and 1K blocks, without metadata checksums, so damage reaches the
    parsing code instead of stopping at a checksum. It has htree directories, a file of 300
    extents and a pending journal transaction;
  - `ext4c.img`: the same with `metadata_csum` and a csum_v3 journal.
- **An iteration:** it damages 1–8 of the sectors that a clean run read: bit flips, bytes, 16/32-bit
  fields set to edge values, bytes copied from another sector, runs of random bytes. Then it
  mounts every volume (journal replay too), walks every directory, reads every file, and on
  writable volumes creates, writes, grows, cuts, renames and deletes files and directories, also
  in damaged ones. It makes the FILESYS service's checks first (read-only volumes, directories),
  so it only makes calls the kernel can make.
- **Two builds:** `fsfuzz` with AddressSanitizer and UndefinedBehaviorSanitizer, and `fsfuzz-stk`
  without sanitizers, whose calls run on a 12 KB stack with an unmapped guard page. The FILESYS
  task has 16 KB, so a recursion too deep for the kernel faults there.
- **A failure** is a crash, a `panic()`, a sanitizer report or a hang over 20 s. Each `FAIL` line
  prints the command that reproduces it:
  `build/host/fsfuzz DISK --seed S --replay I [--dump OUT.img] [-v]`. That command runs the one
  damaged image with the kernel messages shown, and `--dump` writes it out, to boot in QEMU.

`FUZZ_N` (default 1500) is the number of damaged images per disk and build. The six runs go in
parallel, each to its own log in `build/fuzz/*.log`, and it ends with `FUZZ PASS`, in about
4 minutes.

## Tools

| Tool | Purpose |
|---|---|
| `port64/tools/powercut.py` | the power-cut test (`make powercut`) |
| `port64/tests/fsfuzz/fsfuzz.c`, `port64/tools/fuzzdisk.py OUTDIR` | the damaged-disk fuzzer and its disks (`make fuzz`) |
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
- **Kernel panics** print a report:
  - **the reason;** CPU exceptions in the kernel also print a full register dump and the function
    they happened in;
  - **a stack trace** with function names (`kmain+0x219`). The kernel keeps frame pointers, and
    the build links in a table of its functions: a second link with `build/gen/ksyms.c`, made by
    `tools/gen_ksyms.sh`. The build checks that this moves no code.
  - **the crash area:** the report is also saved on the boot disk, with the kernel messages
    before it (the console keeps the last 16 KB). It goes to sectors 1984–2047, in the gap
    before the first partition, if the first partition starts at 2048 or later. GRUB's core on
    a system disk ends far below. The next boot prints the report once (`*** Crash report of an
    earlier boot ...`), and the monitor's `crash` shows it again.
- **Checks built into the kernel:**
  - **stack protector:** `-fstack-protector-strong`, with a canary set at boot. A buffer that runs
    over its function's frame panics with "stack smashing detected in FUNCTION".
  - **freed-page poison:** freed pages are filled with a pattern. A page written after it was
    freed panics when it is handed out again, and freed pages are handed out first. Freeing a page
    twice panics.
  - **`ASSERT(...)`** for internal invariants, and **`kcheck()`**, which walks the scheduler's ready
    queues, the messaging free lists and exchange queues, and the page bitmap against their
    counters. The self-tests run it after each group (`2.4: kernel invariants ok`), and so does
    the monitor's `check`.
- **`make crashtest`** triggers each of these on purpose. Boot 1, with `crash=NAME`, must panic
  with the right message and a stack trace through the right function. Boot 2 must show the saved
  report. Cases: `stack`, `uaf`, `double`, `assert`, `check`, `fault` (a kernel page fault),
  `panic`. About 2 minutes.
- **`make analyze`** runs `gcc -fanalyzer`, `cppcheck` and `clang --analyze` over the kernel
  (`tools/analyze.sh`). Every finding fails it, except those in `tools/analyze.allow` (each with
  its reason). False alarms in the code are marked with a `cppcheck-suppress` comment there.
- **gdb:** `make debug`, then in another terminal:
  `gdb build/kernel.elf -ex 'target remote :1234'`. The kernel is built with `-g`.
- **QEMU monitor:** add `QEMU_EXTRA="-monitor unix:/tmp/doors.mon,server,nowait"` to look at
  registers, memory and the screen from outside (`info registers`, `x/16gx ADDR`).
- **Leaks:** the M5/M6 tests compare the free counts of pages, exchanges, link blocks and request
  blocks before and after running jobs. The monitor's `tasks` command shows the same counts.

## Continuous integration

`.github/workflows/ci.yml` builds DOORS on GitHub Actions (Ubuntu 22.04) for every push and pull
request. It runs `make test`, `smoke`, `powercut`, `fuzz` (500 images per disk), `analyze` and
`crashtest` as parallel jobs, and keeps the logs of a failed job. Weekly, and on demand, it also
runs `make fulltest`, a 20,000-image fuzz and 50 power cuts. To let only passing commits into
`main`, require these checks in the repository's branch protection rules.

## Versions and the changelog

The version is in `port64/VERSION`. The build turns it and the git commit into
`build/gen/doors/version.h`. The kernel prints both at boot (`Version 1.0.0, build v1.0.0`), the
GRUB menu shows the version, and the monitor's `ver` command repeats it. A build from a working tree
with uncommitted changes is marked `-dirty`.

Versions were `0.M.P` while the port was being built (`M` = milestone). 1.0.0 is the release that passes
the smoke test. From there on: MINOR for new features, PATCH for fixes. Every release commit updates
`port64/VERSION` and `CHANGELOG.md` and is tagged `vX.Y.Z` in git.
