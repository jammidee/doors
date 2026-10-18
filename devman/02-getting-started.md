<!-- DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. -->
# 2. Getting started

## Requirements

DOORS is built and run in Linux. On Windows this means WSL with Ubuntu 22.04. The tools are:

| Tool | Used for |
|---|---|
| `gcc`, `ld` (binutils) | the kernel and user programs (freestanding C11) |
| `nasm` | assembly (boot code, interrupt and system call entry) |
| `grub-mkrescue`, `xorriso`, `grub-pc-bin` | the bootable ISO image |
| `qemu-system-x86_64` | the virtual PC DOORS runs on |
| `python3` | test runner and disk image tools |
| `dosfstools`, `mtools`, `e2fsprogs` | building the test disk and checking it afterwards |

## The quickest way: `doors.bat`

On Windows, double-click **`doors.bat`** in the repository root. It:

1. builds the kernel, the user programs and the ISO if anything changed,
2. creates the disk `port64/build/disk.img` if it does not exist,
3. boots DOORS in QEMU and draws the DOORS screen inside the console window.

Type monitor commands at the `>` prompt (see [chapter 3](03-monitor.md)). To stop DOORS, press
**Alt+2** (or **Esc** then **2**) to reach the QEMU monitor, type `quit` and press **Enter**.
The serial log of the session is written to `port64/build/serial-run.log`.

## Building with `make`

In WSL, from `port64/`:

| Command | What it does |
|---|---|
| `make` | builds `build/kernel.elf` and `build/doors.iso` |
| `make console` | boots DOORS drawn in the terminal (QEMU curses). This is what `doors.bat` runs |
| `make run` | boots DOORS in a QEMU window, serial log on the terminal. It needs QEMU's GUI package (`qemu-system-gui`); without it QEMU only opens a VNC server |
| `make run-ext4` | boots DOORS from `build/sysdisk.img`, a disk with GRUB on it and an ext4 C: (see [The ext4 system disk](#the-ext4-system-disk)); no ISO |
| `make test` | runs every self-test headless, then checks the disk with Linux tools ([chapter 10](10-testing-and-tools.md)) |
| `make debug` | like `run`, but QEMU waits for `gdb` on port 1234 |
| `make clean` | deletes `build/` |

`QEMU_EXTRA="..."` adds QEMU options to `run`, `console` and `debug`. For example,
`QEMU_EXTRA="-monitor unix:/tmp/doors.mon,server,nowait"` lets `tools/boot32/qscreen.py --sock
/tmp/doors.mon` read the screen and type keys from a script.

## What you see at boot

```
 DOORS 1.0.0 (x86-64)  Message-based multitasking real-time kernel
Version 1.0.0, build v1.0.0
Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines
Based on MMURTL V1.0 by Richard A. Burgess (public domain)

Memory map: ...
Paging:  own 4-level tables, ...; RAM direct-mapped at ffff800000000000; NX on; ...
Interrupts: GDT+TSS (IST for #DF/NMI), IDT 256 vectors, PIC 0x20-0x2F, PIT 100 Hz, keyboard IRQ1
DOORS: M1 OK
Disk 12: ATA primary master, 128 MB, LBA48 ('QEMU HARDDISK')
Volume C: disk 12 part 1, 40 MB, FAT32 DOORS32
Volume D: disk 12 part 2, 16 MB, FAT16 DOORS16
Volume E: disk 12 part 3, 4 MB, FAT12 DOORS12
Volume F: disk 12 part 4, 64 MB, ext4 DOORS-EXT4
Boot modules: 8
INITIAL.JOB: loaded hello as job 3
Type 'help' for commands. F1 CLI, F2 jobs, F3 stats, Ctrl-Alt-PgDn next screen.
>
```

The first lines give the version and the git commit the system was built from (see
[Versions](10-testing-and-tools.md#versions-and-the-changelog)). The kernel then reports memory,
paging and interrupt setup, finds the disks and their volumes, then
starts the programs listed in `INITIAL.JOB` and opens the monitor.

## The disk

`tools/mkdisk.py` builds the 128 MB disk image with an MBR and four partitions. The FAT volumes
are made with Linux's `mkfs.fat` and the ext4 volume with Linux's `mkfs.ext4`, so DOORS is tested
against file systems it did not create itself.

| Drive | Size | Type | Contents |
|---|---|---|---|
| C: | 40 MB | FAT32 `DOORS32` | `HELLO.TXT`, `A Long File Name.txt`, `DIR1\SUB\DEEP.TXT`, `BIG.BIN`, `BIN\HELLO.ELF`; `DOORS\` (the system directory: `CLI.ELF`, `HELP.CLI`, `COMMANDS.CLI` and every program); `MSAMPLES\READ.ME` |
| D: | 16 MB | FAT16 `DOORS16` | `README.TXT` |
| E: | 4 MB | FAT12 `DOORS12` | `TINY.TXT` |
| F: | 64 MB | ext4 `DOORS-EXT4` | `HELLO.TXT`, `A long file name on ext4.txt`, `MixedCase.Txt`, `BIG.BIN`, `SPARSE.BIN`, `deep\dir\tree\FILE.TXT`, `BIN\HELLO.ELF` |

`make run` and `make console` keep `build/disk.img` between sessions, so your changes stay. Delete
the file to get a fresh disk. It is also rebuilt, and your changes lost, when a program or a file in
`port64/user/sys/` changes, so that `C:\DOORS\` always holds the current programs. `make test`
always builds a fresh `build/disk-test.img`.

## The ext4 system disk

DOORS can also boot from a disk by itself, without the ISO, with its system volume on ext4.
`mkdisk.py --system ext4` (the `build/sysdisk.img` that `make run-ext4` boots) makes:

| Drive | Size | Type | Contents |
|---|---|---|---|
| C: | 64 MB | ext4 `DOORS-SYS` | `boot\` (the kernel, its modules, `INITIAL.JOB`, GRUB's configuration and modules), `DOORS\` (the system directory, as on FAT), `MSAMPLES\READ.ME`, `BIN\HELLO.ELF`, `HELLO.TXT` |
| D: | 16 MB | FAT16 `DOORSXCH` | `README.TXT`: for exchanging files |

GRUB is in the disk's first track: its boot code in the MBR and its core in the sectors before C:.
It reads `boot\grub\grub.cfg` from the ext4 volume and loads the kernel and the programs from
`boot\`. When DOORS has written to C:, GRUB still reads it.

Names on an ext4 C: are case-sensitive, as everywhere on ext4: `dir c:\msamples` doesn't find
`MSAMPLES`, though the drive letter may be lower case. The CLI still finds commands in any case
(`edit` runs `C:\DOORS\EDIT.ELF`). `make run-ext4` keeps the disk between sessions; delete
`build/sysdisk.img` for a fresh one.

## Programs that start at boot

`port64/user/INITIAL.JOB` lists the programs DOORS starts at boot, one name per line. Lines that
begin with `;` are comments. A name refers to a program built from `port64/user/progs/NAME.c` and
put on the boot image as a GRUB module. This is the DOORS form of MMURTL's
`\MMSYS\INITIAL.JOB`.
