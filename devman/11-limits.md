<!-- DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. -->
# 11. Limits and known issues

## System limits

| Item | Limit | Defined in |
|---|---|---|
| jobs | 64 (programs from job 3) | `include/doors/job.h` |
| tasks | 256 | `include/doors/task.h` |
| priorities | 32 (0 = highest; user default 25) | `include/doors/task.h` |
| time slice | 3 ticks = 30 ms | `include/doors/task.h` |
| exchanges | 1024 | `include/doors/msg.h` |
| link blocks (queued messages) | 4096 | `include/doors/msg.h` |
| request blocks | 256 | `include/doors/msg.h` |
| named services | 64 | `include/doors/msg.h` |
| pending timers | 64 | `include/doors/msg.h` |
| kernel stack per task | 16 KB + guard page | `kernel/sched.c` |
| user stack (first task) | 64 KB + guard page | `include/doors/mem.h` |
| open files (whole system) | 128 | `kernel/fsys.c` |
| volumes | 8 | `include/doors/fs.h` |
| path length | 259 characters | `include/doors/fs.h` |
| name length | 255 characters | `include/doors/dirent.h` |
| command line | 79 characters | `kernel/syscall.c` |
| block cache | 1024 sectors (512 KB) | `kernel/bcache.c` |
| disks | ATA devices 12–15 | `drivers/ata.c` |
| CPUs | 1 | |
| monitor input line | 63 characters | `kernel/main.c` |

## Not finished yet

| Area | State | Planned |
|---|---|---|
| CLI, editor, sample programs | not ported. The kernel monitor is the user interface | M9 |
| `malloc` and `free` | not yet: use `AllocPage` | M9 or later |
| Redirection of stdin/stdout to files (`SetSysIn`/`SetSysOut`) | the names are stored, nothing is redirected | later |
| A CPU fault in a program run with `Chain` | ends the whole job; the exit run file (for example the CLI) is not reloaded | later |
| Keyboard LEDs (Caps/Num/Scroll Lock lights) | not driven; the lock states work | later |
| Global key requests from programs (MMURTL's debugger keys) | only Ctrl-Alt-PgDn and Ctrl-Alt-Del are handled | M10 |
| ext4 journal | no journal. A volume that needs recovery mounts read-only | M8b |
| ext4 htree directory writes, deep extent trees on write | `ErcNotSupported` | M8b |
| Booting from an ext4 system disk | programs come from the boot image or a path | M8b/M9 |
| Debugger, disassembler | not ported (job 2 is reserved for it) | M10 |
| `malloc` and a full C library for programs | only `AllocPage` and a small libc | M9 |
| Disk drivers | ATA PIO only; no AHCI/virtio, no interrupt-driven I/O | later |
| Floppy, serial and parallel port services | not ported | later |
| UEFI boot, APIC timers, several CPUs | not started | M10 |

## Differences from MMURTL

Programs written for MMURTL need these changes:

- Recompile for 64-bit with GCC against `doors.h`. `.RUN` files do not run.
- Sizes, file positions, pointers and messages are 64-bit. Messages are two `U64` values.
- File sizes and positions are `U64` (`SetFileSize`, `ReadBlock`, `GetFileLFA`, ...).
- `WriteBlock` extends a file. `DeleteDir` is implemented and refuses non-empty directories
  (`ErcDirNotEmpty`).
- Long file names are allowed everywhere. ext4 names are case-sensitive.
- Bad pointers return errors instead of crashing the system, and a faulting program ends with
  `1000 + vector`.
- `GetDirSector` works only on FAT. Use `ReadDir` for portable directory listing.

## Known issues

- Times on ext4 show in UTC and FAT times in local time (no time-zone setting yet).
- Moving a directory into one of its own subdirectories is not checked yet. Don't do it.
- `make run` opens no window unless QEMU's GUI package is installed. Use `make console`
  or `doors.bat`.
- The problems listed in `PROBLEMS.TXT` for MMURTL V1.0 belong to the 32-bit system. The
  64-bit code was written new, and `DeleteDir`, the lost last block in file copies and crashes from
  bad pointers are fixed.
