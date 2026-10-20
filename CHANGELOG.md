<!-- DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. -->
# DOORS Changelog

Versions are `MAJOR.MINOR.PATCH`. The version lives in `port64/VERSION`. Every release commit is
tagged `vX.Y.Z`. The kernel shows the version at boot and with the monitor's `ver` command, together
with the git commit it was built from (`-dirty` means it has uncommitted changes).

- Until the 64-bit port matches MMURTL, versions are **0.M.P**: `M` is the milestone of `PLAN.md`
  and `P` counts changes within it.
- **1.0.0** will be milestone M9: the CLI, editor and user land running on 64-bit, with the
  smoke test passing.
- After 1.0.0: MINOR for new features, PATCH for fixes. MAJOR changes only if the MMURTL-compatible
  API ever has to break.

## 1.11.0 — 2026-10-21 — growable kernel tables, kernel heap
- **Growable tables** (ROADMAP 3.1, `kernel/gtable.c`): jobs, tasks, exchanges, link blocks,
  request blocks, open files, address spaces, screens and key buffers are no longer fixed arrays.
  Each table has its own 16 GiB slice of kernel address space and maps pages as it grows. Entries
  never move. The old limits are now ceilings: 4,096 jobs, 65,536 tasks, exchanges, request blocks
  and open files, and about one million link blocks. The real limit is free memory.
- **Kernel heap:** `kmalloc`/`kfree` (`kernel/kheap.c`), with size classes up to 2 KB and whole
  pages above that. Blocks come zeroed. A bad or double free panics. The heap is part of `kcheck`.
- **Job numbers are reused:** a new job takes the number of one that ended, once its tasks are
  gone. Before this, every job used up a number, so the system ran out after 64 programs.
- **Boot modules:** up to 64, raised from 16. Before, the 17th and later were dropped without a
  message.
- Monitor command **`tables`**.
- New program `idle`. `make test` runs 1,000 idle jobs at once, three times, and checks that nothing
  leaks (2.4 s under QEMU).

## 1.10.1 — 2026-10-21 — soak test (unfinished)
- **`make soak SOAK_MINUTES=M`** (ROADMAP 2.3, unfinished): in rounds, the kernel supervisor
  (`kernel/soak.c`, kernel command line `soak`) starts a random mix of programs and stops some of
  them partway through. It checks the kernel's invariants each round and its resource counts
  every 10 rounds. Afterwards the host checks the disk.
- **`churn`**, a new program: file work on FAT and ext4 with every read verified, plus memory and
  messages. Its seed comes from its command line (kernel `job_set_cmdline`).
- **Open bug, found by the first run:** stopping a program while FILESYS serves its request can
  crash FILESYS (a page fault in `fsys_task`). `make soak` is therefore not in CI yet; the fix
  belongs to ROADMAP 3.4.

## 1.10.0 — 2026-10-19 — test coverage
- **`make coverage`:** `make test` with a second kernel whose basic blocks mark themselves
  (`-fsanitize-coverage=trace-pc`, `kernel/cov.c`). `tools/coverage.py` turns that into line
  coverage per file (`build/cov/coverage.txt`).
- **First measurement:** 81.3 % of the kernel's 3,041 lines with code are run by `make test`
  (the self-tests' own code not counted).
- The normal kernel is unchanged: the hook only exists with `COV=1`.
- CI runs it too and keeps the report.

## 1.9.0 — 2026-10-19 — Stage 2: analysis, debugging aids, crash reports, CI
- **Static analysis** (`make analyze`): `gcc -fanalyzer`, `cppcheck` and `clang --analyze` over
  the kernel, clean.
  - Found and fixed: the ATA disk name filled all 24 bytes with no NUL, so every print of it read
    past the field.
  - Also: an unchecked out-of-memory case while building the boot page tables, and a string copy
    that read a byte before checking the bound.
  - False alarms were made explicit in the code; one intended finding (the monitor's `div0`) is on
    an allow list.
- **Debugging aids:**
  - the kernel is built with `-fstack-protector-strong`, with a canary set at boot;
  - freed pages are poisoned, and a use after free panics when the page is handed out again;
  - `ASSERT` and `kcheck()` check the scheduler, messaging and page allocator structures (the
    self-tests run it; monitor `check`).
- **Panic reports:** a stack trace with function names (the kernel now carries a table of its
  functions), and for kernel exceptions the function they happened in.
- **Crash reports:** a panic saves its report and the kernel messages before it to the boot disk
  (sectors 1984–2047). The next boot shows it once, and the monitor's `crash` shows it again.
- **`make crashtest`:** each aid triggered on purpose (smashed stack, use after free, double free,
  assertion, broken invariant, kernel page fault, panic), and the report must come back after a
  reboot.
- **CI:** `.github/workflows/ci.yml` runs test, smoke, powercut, fuzz, analyze and crashtest on
  every push, plus weekly long runs. It runs once the repository is on GitHub.

## 1.8.0 — 2026-10-19 — full volumes, empty files, the longest names
- **Security fix in FILESYS:** a path with a drive letter was copied into a 260-byte static buffer
  without a length check. A longer one ran into the next kernel variable (FILESYS's own exchange)
  and hung the file system. Any program could send FILESYS such a `Request` directly. FILESYS now
  checks every length it receives.
- **A full disk is handled cleanly:**
  - FAT and ext4 writes write what fits and report it exactly with `ErcDiskFull`.
  - FAT gives back the clusters a failed grow got (before, the chain stayed longer than the file).
  - ext4 keeps 8 blocks back from file data for the operation's own metadata, and **rolls back
    any operation that fails half-way** (its journal transaction is dropped), so a failure never
    leaves blocks or inodes half-allocated.
- **FAT writes are much faster:**
  - Whole sectors go to the disk in runs of up to 64 in one ATA command.
  - The FAT sectors an operation changes are written once at its end (before its directory
    entry), not 4 sector writes per new cluster.
  - Growing a file with `SetFileSize` zeroes only the old last cluster's tail.
- **`GetVolSpace`** (kernel) and the monitor's **`df`**: size, free space and free inodes. ext4's
  free counts are made exact at mount.
- **Tests:**
  - Every `make test` now covers 0-byte files, 255-character names (12 alike, for FAT's `~N`
    aliases), a refused 256th character and a refused over-long path, a full FAT12/16 root
    directory, an ext4 volume with no inodes left, and writing to the read-only H:.
  - New **`make fulltest`** also fills FAT32, FAT16, FAT12, ext4 (1K) and ext4 (4K) to 100 % and
    back. Free space and inodes must come back exactly, and `e2fsck`/`fsck.fat` must be clean.
  - A small ext4 volume I: (16 MB, 128 inodes) is on the second test disk, which grows to 64 MB.
  - `QEMU_TEST=` runs `make test` with another QEMU (for gdb on a hung test).

## 1.7.0 — 2026-10-19 — M8b done: booting from an ext4 system disk
- **An ext4 system disk that boots by itself:** `mkdisk.py --system ext4` makes a disk with GRUB in
  its first track. C: = ext4 holds `boot\` (the kernel, its modules, GRUB's files) and the system
  directory `\DOORS\`; D: = FAT16 is for exchanging files. `make run-ext4` boots it with no ISO.
  GRUB is installed by hand, as `grub-bios-setup` does it, because WSL has no loop devices.
- **The CLI finds commands in any case:** it tries the name as typed, then upper-cased, so `edit`
  runs `EDIT.ELF` on a case-sensitive ext4 C: too. ext4 names stay case-sensitive.
- **`make smoke` runs on both system disks:** FAT32 C: (booted from the ISO) and ext4 C: (booted
  from the disk). On ext4 a new row, B8, checks the disk afterwards with `e2fsck` and boots it
  again.
- The damaged E: of `make test` is now made only by `mkdisk.py --damage`, so `build/disk.img`
  (`make run`, `doors.bat`) comes up clean.

## 1.6.0 — 2026-10-19 — FAT after a power cut
- **FAT dirty flag:** the first write after mount sets it in the boot sector, where Linux and
  `fsck.fat` keep it. The monitor's `sync` and the end of the test runs clear it. A volume that
  was already dirty at mount stays dirty, as on Linux.
- **The check at mount** (like `chkdsk` at boot) runs on a volume that was dirty, and reports
  without repairing:
  - chains longer or shorter than the file size;
  - chains that reach a free or bad cluster;
  - shared clusters;
  - lost clusters;
  - FAT copies that differ.
  It walks the chains in `fsck.fat`'s order, so both name the same problem for the same file.
- **Read-only when writing is unsafe:** if a chain reaches a cluster the FAT calls free, or two
  files share clusters, the volume is mounted read-only until it is repaired. The first test run
  showed why: DOORS's own FAT12 write test reused such a "free" cluster and cross-linked two files.
  The other problems leave the volume writable. The `Volume` boot line now says `(read-only)`
  for any file system, not only ext4.
- **FAT directories** end at 65,536 entries (FAT's limit), so a directory chain that loops back
  on itself can't keep a walk going forever. A full directory gives `ErcDirFull`.
- **Tests:**
  - `make powercut` adds 5 FAT runs. After each random cut, `fsck.fat -n` must find exactly
    what DOORS's mount check reports, and every finished operation must be on the disk.
  - In `make test`, E: starts out dirty with the harmless kinds of damage, and a new FAT12 volume
    H: on the second disk with the unsafe kinds. check_disk compares `fsck.fat`'s findings with
    DOORS's report for both (new `--log` option) and checks that H: was mounted read-only.
- **Docs:** PROGRESS.md (what changed when, one row per version); ROADMAP Stage 8, networking.
- **Known:** `fsck.fat` 4.2 aborts ("Internal error: next_cluster on bad cluster") on a chain
  that starts with a bad cluster. DOORS reports that case, but there is nothing to compare it with.

## 1.5.0 — 2026-10-18 — damaged disks can't crash DOORS
- **`make fuzz`**, a damaged-disk fuzzer. The kernel's volume, FAT, ext4 and block-cache code runs
  unchanged as a Linux program (`tests/fsfuzz/`) with AddressSanitizer and UBSan, and again on a
  12 KB guarded stack. It damages FAT12/16/32 and ext4 disks (with and without checksums) at
  random, then mounts, reads and writes them. 9,000 damaged images a run.
- **Fixed, found by it** (all in ext4; FAT came through clean):
  - A damaged extent node (entry count, `max`, depth, or an index node without entries) made
    reads and writes run past their buffers, and a child pointer back up the tree made a cycle.
    Every node is now checked as Linux's `__ext4_ext_check` does, and each step down the tree
    must be one level lower.
  - A damaged directory size made directory walks crawl through billions of empty blocks (a
    hang). Holes are now skipped in one step, and a directory has at most 2^32 blocks.
  - A damaged superblock with inode size 0 divided by zero during mount (in the kernel: a
    divide exception, a panic at boot). The geometry is now checked as Linux's `ext4_fill_super`
    does: inode size, blocks and inodes per group, first data block, inode count, first inode,
    descriptor size.
- **Block cache:** lookups go through a hash table instead of scanning all 1024 buffers.
  `make test` went from about 4 minutes to 106 seconds, and the fuzzer got about 100 times faster.

## 1.4.0 — 2026-10-18 — M8b: journals with checksums
- **ext4 volumes whose journal has checksums are written** (JBD2 `csum_v3`, which e2fsprogs 1.47
  makes and Linux turns on when it mounts a `metadata_csum` volume, and the older `csum_v2`). Before,
  they were replayed and then mounted read-only.
  - DOORS writes every CRC32C: the journal superblock's, each descriptor's and commit block's, and
    one per logged block in its tag.
  - The replay checks them all. A block with a wrong checksum ends the log there, so that
    transaction and later ones are not replayed. The journal superblock's checksum is checked at mount.
  - A block number's high half in a csum_v3 tag counts only with `64bit`, as on Linux.
- **Tests:** F: on the test disk now has a csum_v3 journal (`debugfs jo -c`) holding two Linux
  transactions; the second has a damaged block and must not be replayed. `make powercut`
  alternates csum_v3 and plain journals (`mkdisk.py --plain-journal`), so e2fsck checks every
  checksum DOORS wrote.

## 1.3.0 — 2026-10-18 — M8b: deep extent trees on write
- **Extent trees of any depth up to 4** are written: leaf and index node splits, the inode root
  moving down a level when it is full, and truncation at any depth that frees the nodes it empties.
  Before, a file needing more than one index level got `ErcNotSupported`.
- **Faster journal commits:**
  - Only the 512-byte sectors that really changed are logged and checkpointed. A block that was
    written but did not change is not logged at all.
  - The journal superblock's first sector is kept in memory, and a commit rewrites only that
    sector (`s_sequence`, `s_start`).
  - Whole blocks and runs of sectors go to the disk in one ATA command (`bc_write_n`) instead of
    one command and one cache flush per sector.
  - The ext4 stress test went from 99 s to 63 s, before the multi-sector writes.
- **Test:** on G:, a file written as 400 one-block fragments is cut to 200 and grown to 480 (a
  depth-2 tree), and another 400-fragment file is deleted. check_disk verifies the tree with
  debugfs, and `e2fsck -fn` checks it.

## 1.2.0 — 2026-10-18 — M8b: writing big (htree) directories
- **htree directory writes:** names go into Linux's hash-indexed directories through the index:
  half_md4/tea/legacy hashes (signed and unsigned), leaf splits, index node splits, a full root
  becoming two levels, and the dx_tail checksum. Deletes and moves work in indexed directories too.
  Three-level (largedir) indexes are read but give `ErcDirFull` on write.
- **Uninitialised block groups** (`BLOCK_UNINIT`, `INODE_UNINIT`): their bitmaps are built on
  first use (as Linux does), so a fresh mkfs volume can be filled completely.
- **Second test disk:** a GPT disk with a 1K-block ext4 volume (G:) holding Linux-indexed
  directories (one and two levels). It is the first test of DOORS's GPT support, and it passes.
  check_disk runs `e2fsck -fn` on it and compares every name.
- Test fixes: check_disk and powercut.py ignore unused directory entries (inode 0) that `debugfs
  ls -p` lists.

## 1.1.0 — 2026-10-16 — M8b: the ext4 journal
- **Journaled ext4 writes** (JBD2, ordered mode): every modifying file operation is one
  transaction. Metadata goes through the journal; file data goes to disk first.
- **Journal replay at mount**: DOORS's own journal, or one Linux left (64-bit, csum v2/v3 tags,
  revoke records). Journals with checksum features are replayed and the volume mounted read-only.
- `needs_recovery` is set while mounted (as on Linux); the new monitor command `sync` marks the
  volumes clean.
- **Power-cut test** (`make powercut`, `tools/powercut.py`): QEMU killed at random points and inside
  a commit; Linux's e2fsck replay and DOORS's replay must both leave a clean volume with every
  finished operation present.
- The test disk carries a committed but unreplayed Linux journal transaction (`debugfs jw`); DOORS
  replays it at boot. check_disk verifies that the journal is empty and the volume clean after the
  tests.

## 1.0.0 — 2026-10-16 — M9: DOORS on 64-bit matches MMURTL
- **The smoke test passes on 64-bit** (`make smoke`, `tools/smoke64.py`): 24 rows pass, P7 (the
  debugger) is deferred to M10. Every row that hit a known MMURTL bug on 32-bit (copy, rd, the
  editor's file name, undel, FAT timestamps, dump) now passes.
- `docs/SMOKE_TEST.md` has the 64-bit column filled in.
- From here on versions follow semantic versioning: MINOR for features, PATCH for fixes.

## 0.9.3 — 2026-10-16 — M9 step 9d: editor and samples
- MMURTL's editor (`MSAMPLES/EDITOR/EDIT.C`) ported as `edit`. The file name is no longer cut
  to 13 characters (smoke test P2b).
- Samples ported: BusyLoop, Service (NUMBERS) and TestSvc.
- The kernel's `"KEYBOARD"` service: code 1 reads a key through a request (what MMURTL's Service
  sample uses), code 4 is accepted.
- **Fix:** file handles start at 1. MMURTL programs (the editor) and the CM32 library treat
  handle 0 as "no file"; DOORS gave 0 to the first open file.
- Tests: an editor session in the CLI test (create, type, save, quit; the long name is checked on
  the host), NUMBERS + 2 testsvc clients, BusyLoop + Ctrl-Alt-Del, no pages leaked.
- `make test`: the test disk uses QEMU `cache=unsafe` (no host fsync per flush; the disk is rebuilt
  each run), timeout 240 s.

## 0.9.2 — 2026-10-16 — M9 step 9c: the CLI
- MMURTL's CLI (`MSAMPLES/CLI/CLI.C` V1.0) ported as `user/progs/cli.c`: Cls, Copy, Del, Dir,
  Dump, Exit, Help, MD, Monitor, Path, RD, Ren, Run, Type, and external commands run with `Chain`
  (the CLI comes back as the exit run file).
- Fixes over MMURTL's CLI: Copy keeps the last partial block (smoke test F5), Dump's ASCII column,
  the command file is closed, Dir works on ext4 with long names.
- System directory `C:\DOORS\` on the disk (CLI.ELF, HELP.CLI, COMMANDS.CLI and every program;
  `mkdisk.py --progs --sys`). The monitor's F1 loads the CLI and gives it the screen; programs from
  INITIAL.JOB get the screen too (as MMURTL's Monitor did).
- File system: names without a drive are relative to the job's path (MMURTL FSYS.C `BuildSpec`).
  `LoadNewJob`/`Chain` find a bare name as a file if no boot module has it.
- Test: the CLI driven by typed commands (md, copy, an external command, dir, exit).

## 0.9.1 — 2026-10-16 — M9 step 9b: job calls and C library
- `Chain` and the exit run file (`SetExitJob`/`GetExitJob`): a program can be replaced inside its
  job, and `ExitJob` reloads the exit run file. That is how the CLI comes back after a program.
- `SetPath`/`GetPath`, `SetUserName`/`GetUserName`, `SetSysIn`/`SetSysOut`/`GetSysIn`/`GetSysOut`,
  `GetCMOSTime`/`GetCMOSDate` (BCD, as MMURTL).
- C library for programs: `<stdio.h>` (printf family, `fopen`/`fgets`/... over stream files),
  `<string.h>`, `<ctype.h>`, `<stdlib.h>`.
- **Fix:** `Compare`/`CompareNCS` now return -1 when the bytes match, as in MMURTL. DOORS returned
  the length, which MMURTL programs (the CLI's command lookup) would read as a mismatch.
- Make keeps user object files (they were deleted as intermediates and relinked every time).
- New `jobtest` program (three programs in one job).

## 0.9.0 — 2026-10-16 — M9 step 9a: screens and keyboard
- Every job has its own 80×25 virtual screen (MMURTL `VIDEO.ASM`): `SetVidOwner`, `GetVidOwner`,
  `ClrScr`, `TTYOut` with attributes, `PutVidChars`, `PutVidAttrs`, `GetVidChar`, `SetXY`, `GetXY`,
  `ScrollVid`, `SetNormVid`, `GetNormVid`, `Beep`, `Tone`; `EditLine` in the user library.
- Keyboard service as in `KEYBOARD.ASM`: MMURTL key codes with shift/lock state, keys go to the
  job on screen, `ReadKbd`. Global keys: Ctrl-Alt-PgDn (next screen), Ctrl-Alt-Del (kill the job on
  screen).
- Monitor: status line with version, clock and ticks; F1 CLI, F2 jobs, F3 stats; `screen N`.
- ATA: a cache flush may take up to 30 s (QEMU fsyncs the image on the host; 3 s was sometimes
  too short).
- Test runner: `{name}` raw keys in `--send` (`{f1}`, `{up}`, `{backspace}`). New `vidtest` program.

## 0.8.1 — 2026-10-16
- Versioning: `port64/VERSION`, version and build commit in the boot banner, GRUB menu and
  monitor `ver` command; this changelog; tags for all earlier milestones.
- DOORS System Manual in `devman/`.

## 0.8.0 — 2026-10-16 — M8: ext4
- ext4 read/write behind a new VFS layer (Linux default features: 64bit, extents, flex_bg,
  metadata_csum); files over 4 GB, sparse files, case-sensitive long names.
- `ReadDir`, system call 110, the first DOORS-only call. The monitor's `dir` works on every file system.
- Random file-system stress test. Host `e2fsck`/`debugfs` checks after every test run.
- `doors.bat` and `make console`: run DOORS in a Windows console.

## 0.7.0 — 2026-10-16 — M7: disks and FAT
- ATA PIO LBA48 driver, MBR/GPT partitions, block cache, drive letters from C:.
- FAT12/16/32 with long names, the FILESYS service and every MMURTL file call (64-bit sizes).
- `DeleteDir` implemented (unfinished in MMURTL). Programs load from files.

## 0.6.0 — 2026-10-16 — M6: user mode and jobs
- Ring 3, `SYSCALL`/`SYSRET` in MMURTL's call numbering, ELF64 programs, `INITIAL.JOB`.
- Every user pointer checked. A faulting program ends with error 1000 + vector and the system keeps running.
- User-mode services, `KillJob`, leak-free job teardown.

## 0.5.0 — 2026-10-16 — M5: messaging
- Exchanges, `SendMsg`/`WaitMsg`/`CheckMsg`, `Request`/`Respond`/`MoveRequest` with buffer
  aliasing, named services, `Alarm`/`KillAlarm`. The keyboard delivers keys as messages.

## 0.4.0 — 2026-10-16 — M4: tasks
- Software context switch with FPU/SSE state, 32-level priority scheduler with 30 ms time
  slices, `Sleep`, `NewTask`/`SpawnTask`/`SetPriority`.

## 0.3.0 — 2026-10-16 — M3: memory
- Physical page allocator, own 4-level page tables with NX and guard pages, per-job address
  spaces, `AllocPage`/`AllocOSPage`/`AliasMem` and the other MMURTL memory calls.

## 0.2.0 — 2026-10-16 — M2: interrupts and time
- IDT, exception reports with register dumps, PIC, 100 Hz PIT, PS/2 keyboard.

## 0.1.0 — 2026-10-16 — M1: long mode
- DOORS boots in x86-64 long mode from GRUB (Multiboot2) and prints its banner to VGA and serial.

## M0 (no version) — 2026-10-15/16
- The original 32-bit MMURTL V1.0 boots and passes the smoke test in QEMU. That is the baseline.
