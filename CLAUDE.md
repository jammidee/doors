# CLAUDE.md

## What this is

**DOORS** is the operating system developed in this repository (jammios). It was formerly MMURTL:
it starts from MMURTL V1.0, Richard A. Burgess's message-based, multitasking, real-time 32-bit x86
protected-mode OS from the book *Developing Your Own 32-bit Operating System* (1991–1999,
now public domain). Everything here is DOS-era: 8.3 upper-case filenames, `.BAT` builds,
tools that run under MS-DOS.

**Naming:** use **DOORS** for our OS, including docs, new code, banners and the 64-bit port. Use **MMURTL**
only for Burgess's original V1.0 code and its real file names (`MMURTL.RUN`, `MMURTL.ATF`, …), which are
not renamed.

**Active goal:** port DOORS to x86-64 (long mode). [PLAN.md](PLAN.md) is the execution plan:
milestones M0–M10, acceptance checks, workflow. [docs/PORT64_PLAN.md](docs/PORT64_PLAN.md) is the
technical design. Read both before doing any porting work, and keep the milestone table and the phase
checklists up to date. Decisions already made:
- Keep every public API name, parameter order and `Erc*` code. Only types widen.
- Native file system is modern (ext4 subset) behind a VFS. FAT stays for interchange.
  The `MFILES.H` file API is kept, with sizes and offsets widened to 64-bit.

## Dev log

Log your activities in `devlogs/YYYY-MM-DD.md`, one file per day; append if it already exists. Record what was
done, what was found (bugs, surprises, error codes and their meaning), decisions with their reasons, and
next steps. Keep it factual and include file:line references and commit hashes.

## Layout

| Path | Contents |
|---|---|
| `OSSOURCE/` | The kernel and built-in drivers/services. `MMURTL.ATF` lists every module and the link order. |
| `CM32/` | CM32 C compiler (source + `CM32.EXE`) and its small libc (`LIB/`, `INCLUDE/`). Emits DASM-syntax `.ASM`. |
| `DASM/` | DASM assembler/linker. Consumes `.ATF` template files, produces `.RUN` executables. |
| `MSCODE16/` | 16-bit DOS tools (Borland C / TASM): `MMLOADER` (boots MMURTL from DOS), boot sector + image maker, `DUMPRUN`, `READOBJ`. |
| `MMSYS/` | Ready-to-run system directory (`MMURTL.RUN`, `CLI.RUN`, `EDIT.RUN`, `INITIAL.JOB`). |
| `MSAMPLES/` | Sample apps (CLI, editor, dumbterm, print, service, …) and MMURTL-hosted builds of CM32 and DASM. |
| `fp32/` | Later addition: real-number math library for CM32. Not part of the original distribution. |
| `PROBLEMS.TXT` | Author's list of known bugs and unfinished features. |
| `port64/` | DOORS 64-bit (see below). |
| `devman/` | DOORS System Manual (Markdown, 11 chapters). Update it when commands, calls, error codes, limits or tools change. |
| `CHANGELOG.md` | DOORS version history. |
| `PROGRESS.md` | What changed when: one row per version (date, commit, PLAN/ROADMAP step, devlog). |
| `ROADMAP.md` | The way from 1.x to a stable OS: stages, steps and the test that proves each one. |
| `PROBLEM-SOLN.md` | PROBLEMS.TXT side by side with what DOORS has solved. |

## Original build (32-bit, under DOS)

```
cd OSSOURCE
MAKEALL.BAT     # CM32 each *.C with /L, then DASM MMURTL.ATF /E (runs twice), copy to \MMSYS
```

- `CM32 file.C [out.ASM] /L` → `.ASM` (`/L` list errors, `/E` embed source, `/S` silent).
- `DASM file.ATF [out.RUN] /E` (`/L` full listing, `/E` errors only).
- `.ATF` files are `.INCLUDE` lists. In `MMURTL.ATF` the first 5 includes
  (`MOSIDT`, `MOSGDT`, `MOSPDR`, `MPUBLICS`, `MAIN`) **must stay in that order**, because
  they fix physical addresses of the IDT, GDT, page directory and call table.
- **Run it (no DOS needed):** in WSL, run `bash tools/boot32/run.sh [--rebuild]`. It builds a boot floppy (A:)
  and a FAT16 IDE disk (C:) under `build/boot32/` and opens QEMU in a window.
  - Read the screen or send keys with `python3 tools/boot32/qscreen.py [--keys "d i r ret"] [--cmd quit]`.
  - In the Monitor, F1 = load CLI.
- Shipped boot files are unreliable. `MSCODE16/BOOTCODE/MMURTL.IMG` is an older kernel, and `BOOTSECT.IMG`
  has a bad BPB. The tools rebuild both from `MMSYS/MMURTL.RUN` and `BOOTBLOK.EXE`. See `devlogs/2026-10-15.md`.
- There are no tests. Verification means booting it and exercising the CLI.

## DOORS 64-bit build (`port64/`)

In WSL (Ubuntu 22.04: gcc, nasm, ld, grub-mkrescue, xorriso, qemu):

```
cd port64
make          # build/kernel.elf + build/doors.iso (GRUB, Multiboot2)
make run      # QEMU window; serial log on the terminal. Add QEMU_EXTRA="-monitor unix:/tmp/doors.mon,server,nowait"
              #   to read the screen with tools/boot32/qscreen.py --sock /tmp/doors.mon
make console  # the DOORS screen drawn in the terminal (QEMU -display curses); `doors.bat` at the repo root runs
              #   this from Windows. WSL's QEMU has no GTK/SDL module, so `make run` has no window unless
              #   qemu-system-gui is installed. Quit curses with Alt+2, then `quit`.
make run-ext4 # boot build/sysdisk.img: GRUB on the disk, C: = ext4 system volume (mkdisk.py --system ext4), no ISO
make smoke    # docs/SMOKE_TEST.md on 64-bit (tools/smoke64.py): types into the CLIs, reads the screen; runs twice,
              #   FAT32 C: from the ISO and ext4 C: from the disk (--ext4); ~3 min
make powercut # kills QEMU during journaled ext4 writes (tools/powercut.py), checks Linux and DOORS replay; ~2 min
make fulltest # make test + every volume filled to 100 % and freed (kernel cmdline "test full"); ~5 min.
              #   QEMU_TEST=script runs another QEMU for make test/fulltest (e.g. one adding -s, to gdb a hang;
              #   qemu_test.py logs to /tmp/doors-test-PID.log while it runs). KVM in WSL is slower (nested PIO).
make fuzz     # damaged FAT/ext4 disks against kernel/{vol,fat,ext4,bcache}.c built for Linux with ASan/UBSan and a
              #   12 KB guarded stack (tests/fsfuzz); ~4 min. A FAIL line prints its --replay command
make test     # headless (tools/qemu_test.py); passes if the serial log has every --expect line in TEST_ARGS and the kernel
              #   exits QEMU via isa-debug-exit (status 33). Kernel cmdline "test" enables the exit.
make debug    # QEMU waits for gdb on :1234
```

- Layout: `boot/` (Multiboot2 + 32→64 switch), `kernel/`, `drivers/`, `arch/x86_64/`, `include/doors/`.
- Memory model: kernel linked at `0xFFFFFFFF80000000` (`-mcmodel=kernel`). The boot code is at physical 1 MiB.
  - Until `vmm_init()`, the boot tables map the first 1 GiB at 0 and at the higher half (`PHYS_TO_VIRT`, boot only).
  - After it, the kernel's own tables apply. Use `phys_to_virt()` (direct map at `HHDM_BASE`, `include/doors/mem.h`).
    The low identity map is gone. `.text` is RX, `.rodata` R, the rest RW+NX (CR0.WP on), and the page under the
    kernel stack is an unmapped guard.
  - Anything that held a boot-mapped pointer must be remapped at the switch (see `pmm_remap`, `vga_remap`).
  - Layout: user `0x40000000..`, direct map `0xFFFF800000000000`, OS pages `0xFFFFC00000000000`, kernel image `KERNEL_VMA`.
- Memory calls keep MMURTL's names and error codes (`AllocPage`, `AllocOSPage`, `AliasMem`, … in `kernel/mem.c`).
- Use `probe_read`/`probe_write` to touch memory that may not be mapped. They return false instead of faulting.
- When editing C with escapes (`\n`, `'\0'`), use the Edit tool, not Python/sed one-liners: escapes got mangled
  that way twice (a literal NUL byte ended up in a source file).
- Code is freestanding C11 with `-mgeneral-regs-only` (no SSE in the kernel yet) and `-Werror`.
- Kernel messages go through `kprintf` to the VGA screen and COM1. Each milestone adds self-tests in
  `kernel/selftest.c` (run on cmdline `test`) and `--expect`/`--send` lines to `TEST_ARGS` in the Makefile.
- No libc: `kernel/string.c` provides memcpy/memset/strcmp etc. (GCC may emit calls to them itself).

## Architecture facts that matter for the port

- **Tasks are hardware TSSs.** Context switch = `JMP FWORD PTR [TSS]` (`KERNEL.ASM`).
  The TSS layout in `TSS.INC` is extended past the CPU-defined 104 bytes with OS fields
  (`TSS_Exch`, `TSS_pJCB`, `Priority`, `NextTSS`, …), so it is also the task control block.
- **System calls are GDT call gates.** `MPUBLICS.ASM` maps each API to a gate selector
  (`_WaitMsg` = 40h, `_SendMsg` = 48h, …, every 8 bytes). Callers do `CALL FWORD PTR _Name`.
  Selector values are ABI and must not move.
- **Calling convention for OS calls is Pascal-like:** args are pushed left to right and the callee
  pops them (`RETF n`). With a far call, the *last* argument is at `[EBP+12]`. C prototypes are
  declared `extern far` (`MKERNEL.H`, `MJOB.H`, …).
- **Memory:** flat 4 GB segments at selectors 08h/10h/18h with 2-level paging.
  Each job has its own page directory, and memory is shared between jobs by aliasing
  (`AliasMem`/`DeAliasMem`). The OS runs in the context of whichever task called it; it is not a task itself.
- **IPC core:** exchanges, link blocks, request blocks (`RQB.INC`), `Request`/`Respond`/
  `WaitMsg`/`SendMsg`/`CheckMsg`. Services register by name (`RegisterSvc`).
- Error codes are 32-bit, `0` = OK, defined in `MOSEDF.INC` (`Erc*`).

## Versioning

- `port64/VERSION` holds `MAJOR.MINOR.PATCH`. It was `0.M.P` until the port matched MMURTL (`M` = milestone);
  `P` = changes within it. M9 (user land, smoke test passing) = `1.0.0`. After that, semantic versioning.
- Every commit that changes the system adds a row to `PROGRESS.md` (date, version, commit, PLAN/ROADMAP step,
  devlog; a commit can't contain its own hash, so the next commit fills it in), bumps the version, adds a `CHANGELOG.md` entry, and is tagged
  `vX.Y.Z` (annotated). Past milestones are tagged v0.1.0 (M1) … v0.8.0 (M8).
- The Makefile generates `build/gen/doors/version.h` (`DOORS_VERSION`, `DOORS_COMMIT` from
  `git describe --always --dirty`). Only `kernel/main.c` includes it: the banner, GRUB menu and the monitor's
  `ver` command show it.

## Conventions

- **Copyright:** DOORS OS is copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. Every DOORS
  file (`port64/`, `tools/`, and any new source, header, assembly, linker script, Makefile or script) starts
  with the line `DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines.` in its comment
  syntax (after a `#!` line if there is one). Burgess's original MMURTL V1.0 files are public domain and keep
  their own headers.

- Treat original files as the reference. Don't reformat them, rename them, or "modernize" them in place.
  New 64-bit code goes in a separate tree (see the plan). Preserve the copyright headers.
- `.ASM` files generated from `.C` (FLOPPY, FSYS, HARDIDE, JOBC, MONITOR, PARALLEL, RS232,
  UASM, and the CM32 lib) are build outputs. Edit the `.C` file, not the generated `.ASM`.
- Match surrounding style: tabs, upper-case mnemonics, `;` comments in ASM; K&R-style C.
- Windows host: shell is PowerShell, with Git Bash also available. Paths in old sources are DOS
  absolute (`c:\cm32\...`).
- Git: `.gitattributes` stores the original tree byte-for-byte (`-text`). New files under `tools/`, `port64/`
  and `*.md` use LF.

## DOORS kernel conventions (port64)

- Scheduler (`kernel/sched.c`, `include/doors/task.h`): priority 0 = highest; a 30 ms slice among equal
  priorities; strictly higher priority runs at once. The scheduler is locked by `irq_save`/`irq_restore`
  (`x86.h`), which is the only lock (single CPU).
- Code that can block (Sleep, later WaitMsg) must run in task context, never in an interrupt handler.
  Interrupt handlers may make tasks ready; the timer may switch tasks inside its IRQ (the EOI is sent first).
- Waiting loops in tasks should block (`Sleep`, later `WaitMsg`) rather than `hlt`, so lower priorities can run.
- Messaging (`kernel/msg.c`, `kernel/rqb.c`, `include/doors/msg.h`): MMURTL names, parameters and error codes
  (`include/doors/erc.h` holds all error codes). Messages are two U64. Use `ISendMsg` in interrupt handlers and
  `SendMsg` in tasks. A service reads `struct RqBlk` from the handle it receives; its `pData1`/`pData2` are
  already aliased into the service's job.
- Screens and keyboard (`kernel/video.c`, `kernel/kbdsvc.c`, `drivers/keyboard.c`, `include/doors/video.h`):
  every job has an 80×25 virtual screen; `kprintf` writes job 1's (row 0 = status line). Video calls act on
  `current->job`. Keys go to the video owner's 64-key buffer. Use `kbd_inject(code)` to fake keys in self-tests,
  and `{name}` raw keys (`{f1}`) in `--send`. The old ASCII hooks `kbd_getc`/`kbd_set_exch` exist only for
  the M2/M5 tests, which run before `kbd_service_start()`.
- Every task has its own exchange (`GetTSSExch`). `Sleep` = `Alarm` on it + `WaitMsg`, so don't use a task's
  own exchange for other traffic.
- User programs (`port64/user/`): static ELF64 at 0x40000000, written against `user/lib/doors.h` (MMURTL names)
  and the C library `user/lib/libc.c` (`<stdio.h>`, `<string.h>`, `<ctype.h>`, `<stdlib.h>` in `user/lib/libc/`).
  `Compare`/`CompareNCS` return -1 on a match, as MMURTL's do (not the length).
  `Chain`/`ExitJob` with an exit run file replace the program inside the same job (`replace()` in `kernel/job.c`).
  Add a program as `user/progs/NAME.c`; the Makefile builds it and adds it to the ISO as boot module NAME, which
  `LoadNewJob("NAME", ...)` and the monitor's `run NAME` find. `user/INITIAL.JOB` lists what starts at boot.
- New system calls: add a `case SYS_Name` in `kernel/syscall.c` (check every user pointer with
  `urange`/`copy_in`/`copy_out`) and a stub in `libdoors.c`. Never edit `syscalls.def` by hand; it is generated.
  MMURTL's calls are 0–109. A DOORS-only call goes at the end of the list in `tools/gen_syscalls.sh`
  (`ReadDir` = 110) and is never renumbered.
- ext4 journal (`kernel/ext4.c`, "journal" section): in ext4.c, write **metadata** with `mwr()`/`secwr()`
  (they go into the running transaction) and **file data** with `bwr()` (direct, ordered mode); read with
  `brd()`/`secrd()`, which see the transaction. A new modifying `fs_ops` entry must be wrapped in
  `tx_begin`/`tx_end` like `j_create`. Never write the superblock directly while mounted (the power-cut test
  tore it); `ext4_unmount()` clears needs_recovery. Run `make powercut` after touching any of this.
- ext4 operations are atomic: `tx_end_rb` rolls the transaction back (and `e->sb`) when an operation fails, unless
  it set `tx.clean_stop` (a write that ran out of data blocks between blocks: committed, partial). Data blocks are
  only given out above `DATA_RESERVE` free blocks. A new failure path inside an operation needs no cleanup code.
- FAT: FAT sectors an operation changes wait in a queue (`fat_rd`/`fat_wr`, `fq`) and are written by `fat_flush`:
  before every directory entry write (`dir_write`) and at the end of each `fat_ops` operation (the `fx_*` wrappers).
  Read and write FAT sectors only through `fat_rd`/`fat_wr`. Whole data sectors go out in runs (`wr_n`).
- Requests are untrusted too: FILESYS checks every length from a requester (`build_spec`), since programs may send
  it a `Request` directly, not only through the checked system calls.
- On-disk values are untrusted: range-check every count, size, block number and depth before using it as an index,
  loop bound or divisor (`ext_node_ok`, `dir_nblk` and the superblock checks in `ext4_mount` are the patterns).
  Run `make fuzz` after changing vol.c, fat.c, ext4.c or bcache.c. The file system code must stay free of
  kernel-only calls beyond the ones `tests/fsfuzz/fsfuzz.c` stubs (kprintf, panic, AllocOSPage, clocks, blk_io).
- WSL ends the distro shortly after the last `wsl.exe` session closes, killing `nohup`/`&` jobs and clearing
  `/tmp`: start a long job and wait for it in the same session.
- MMURTL ports (`MMURTL_PORTS` in the Makefile: cli, edit, busyloop, service, testsvc) are copies of the
  MSAMPLES sources with the changes listed in each header; they compile with `-Wno-pointer-sign` and a few
  other relaxed warnings. File handles are 1..128 (0 = "no file" in MMURTL code). The kernel's `"KEYBOARD"`
  service (codes 1 and 4, `kernel/kbdsvc.c`) serves MMURTL programs that request keys.
- System directory `C:\DOORS\` (MMURTL's `\MMSYS\`): mkdisk copies every `build/user/*.elf` (`--progs`) and
  `user/sys/*` (`HELP.CLI`, `COMMANDS.CLI`, `--sys`) there. The CLI is `user/progs/cli.c`, a port of
  `MSAMPLES/CLI/CLI.C` (the original stays untouched; its header lists every change). File names without a
  drive are relative to the job's path (`build_spec` in `kernel/fsys.c`).
- ext4 system disk (`mkdisk.py --system ext4`, `make run-ext4`): GRUB is installed by hand, as grub-bios-setup
  would (no loop devices in WSL): copy only bytes 0..0x1B7 of `boot.img` into the MBR (its tail overlaps the
  partition table: GRUB then says "no such partition"), put core.img's LBA at 0x5C, core.img (grub-mkimage) in
  the first track. ext4 names are case-sensitive on purpose; the CLI upper-cases command names as a fallback.
  Test fixtures that damage a disk (`--damage`) belong to `make test` only, never to `build/disk.img`.
- Disks and files: `make test` builds `build/disk-test.img` (tools/mkdisk.py: MBR + FAT32 C:, FAT16 D:,
  FAT12 E:, ext4 F: with a csum_v3 journal; `--plain-journal` for none) and `build/disk2-test.img` (`--disk2`: GPT, 1K-block ext4 G: with htree directories made
  by `e2fsck -D`), and afterwards `tools/check_disk.py` verifies both with `fsck.fat`/mtools and
  `e2fsck -fn`/`debugfs`. Note: `debugfs ls -p` also lists unused entries (inode 0); skip them. `make run` uses
  `build/disk.img` (kept between runs; delete it for a fresh one).
- File calls go through the FILESYS service (`kernel/fsys.c`) and keep MMURTL's names and modes. A request's
  receive buffers (index ≥ npSend) must be writable in the requester's space; `rqb_received` checks this, so
  set npSend correctly when a kernel service writes into a caller's buffer.
