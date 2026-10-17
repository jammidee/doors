<!-- DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. -->
# MMURTL's known problems and DOORS

`PROBLEMS.TXT` is Richard Burgess's list of known problems in MMURTL V1.0 (1995–1999). This page
puts each item next to what the 64-bit DOORS port (`port64/`, version 1.1.0) does about it.

**Status:** ✅ solved · ➖ no longer applies · ⚠ partly · ❌ still open

## 1. Loading and running MMURTL

| PROBLEMS.TXT | Status | DOORS |
|---|---|---|
| MMURTL may not run with **EMM386** loaded: it "doesn't take kindly to hostile memory take-overs". | ➖ | DOORS doesn't start from DOS. GRUB loads it (Multiboot2) and it switches to 64-bit long mode itself, so no DOS memory manager is involved. |
| **a)** Problems with some newer 32-bit disk controllers that move data directly to memory; untested with UDMA 33/66 PCI controllers. | ⚠ | The new ATA driver (`drivers/ata.c`) uses PIO with 48-bit addresses (disks over 128 GB), checks timeouts and drive errors, and allows a slow cache flush 30 s. It has only been tested on QEMU's IDE controller. DMA, AHCI (SATA) and virtio disks are not written yet. |
| **b)** SCSI disks and devices are not supported. | ❌ | Still no SCSI driver, and no AHCI or NVMe either. |
| **c)** Disks compressed by MS-DOS or Stacker are not supported. | ➖ | Still not supported, but no longer relevant: DOORS reads FAT12/16/32 (with long names) and **ext4**, the disk formats in use today. |
| **d)** MMURTL doesn't recognise OS/2's multiboot partition correctly. | ✅ | DOORS reads MBR partition tables and **GPT** (tested on a second test disk), finds FAT and ext4 by their contents, and reports other Linux partitions without touching them. Booting goes through GRUB, which handles multi-boot setups. |

## 2. What's not done

| PROBLEMS.TXT | Status | DOORS |
|---|---|---|
| **File system a)** "The file system call to remove a directory is not completed", so it was left out. "Use MS-DOS to get rid of directories." (On the 32-bit system, `rd` hangs the whole machine: smoke test F7.) | ✅ | `DeleteDir` is implemented on FAT and ext4. It refuses a directory that isn't empty (`ErcDirNotEmpty`, a new error code), and the CLI's `rd` works (smoke test F7 passes). |
| **DLLs** are not supported, "and never will be". | ➖ | Unchanged, by the same choice: DOORS programs are static ELF64 executables. |
| **Device drivers** can't be loaded dynamically; they must be built into the OS. | ❌ | Unchanged: drivers are still built into the kernel. |
| **DASM:** the `.SEARCH` library command is not fully implemented. | ➖ | The 64-bit DOORS is built with GCC, NASM and GNU ld, which search libraries normally. DASM and CM32 themselves are not ported yet (planned for M10), so the original problem remains in the 32-bit tools. |
| **CLI:** no wildcards (`*.*`), so copy, rename and delete take single file names. | ❌ | Still single names. The DOORS CLI is MMURTL's CLI ported with as few changes as possible, and wildcard matching hasn't been added. |

## 3. Bugs

| PROBLEMS.TXT | Status | DOORS |
|---|---|---|
| **a)** Sometimes, while a program is "beating the filesystem to death", loading another CLI crashes the system with a processor double fault, before it even reaches the debugger. | ⚠ | Not seen in DOORS. The kernel is new code: software task switching instead of hardware task switching, a separate stack for double faults (so one is reported with a register dump instead of silently resetting), and guard pages that catch stack overflows. Heavy file system use is tested (a random stress test of 400 operations, and power cuts during writes), and so is starting several CLIs. The exact scenario, a new CLI loaded *during* a file system stress run, has no dedicated test yet. |
| **b)** Ctrl-Alt-Delete is "a rather nasty way to shut down an application". Afterwards, switching to other applications sometimes stops working. | ✅ | Ctrl-Alt-Del is handled by the kernel's keyboard task, which never runs inside the interrupt. It ends the job on screen with the full teardown: tasks, exchanges, services, requests, open files, memory. The screen and keyboard go back to the monitor. Self-tests check that no memory leaks. The smoke test kills BusyLoop this way and then switches screens normally (rows P3, P6, B7). |
| **CM32 C compiler a)** Accidentally nesting a function sends the compiler into an endless loop. | ➖ | DOORS 64-bit code is compiled with GCC. CM32 is not ported yet (M10), so the bug remains in the original 32-bit CM32. |

## 4. The updated file system

| PROBLEMS.TXT | Status | DOORS |
|---|---|---|
| An updated file system was available for download but not built in, so that the code would follow the book exactly. | ✅ | DOORS has a new file system service with the same MMURTL calls, names and error codes, plus 64-bit file sizes. It supports FAT12/16/32 with long file names, and **ext4** (the Linux format) with a journal that survives power cuts. Linux's own checking tools verify both after every test run. |

## Other MMURTL bugs found and fixed while porting

These are not in `PROBLEMS.TXT`, but the 32-bit smoke test (`docs/SMOKE_TEST.md`) and the port
turned them up:

| Bug in MMURTL V1.0 | DOORS |
|---|---|
| FAT timestamps are garbage: `GetFATTime()` in FSYS.C is empty. | Real dates and times from the CMOS clock. |
| The CLI's `copy` loses the last partial 512-byte block of a file (`Error 00217`). | Copies are byte for byte (smoke test F5). |
| The editor keeps only 13 characters of a file name from the command line (`C:\SMOKE\HELLO.TXT` is saved as `HELLO`): `strncpy(Filename, name, 13)` in EDIT.C. | Whole file names are kept (smoke test P2b). |
| `HELP.CLI` lists an `undel` command that the CLI doesn't have. | Removed from the DOORS HELP.CLI. |
| `EditLine` writes one byte past the end of a full line. | Fixed in the DOORS user library. |
| The CLI's `dump` shows a junk character at the end of each line's text column (`0x0A` shown as `.e`). | Fixed. |
| The disk head count is taken from the partition table instead of the drive (wrong disk geometry). | DOORS uses the drive's own LBA addressing and never relies on cylinder/head/sector geometry. |
| A bad pointer passed to a system call can crash the whole system. | Every pointer is checked; the call returns `ErcBadLinAdd` and the program keeps running. A program that faults on its own is ended with error `1000 + exception number`, and the system goes on. |

See `CHANGELOG.md` for when each of these changed, and the DOORS System Manual (`devman/`) for how
things work now.
