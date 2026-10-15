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
