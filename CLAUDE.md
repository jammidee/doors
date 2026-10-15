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

## Conventions

- Treat original files as the reference. Don't reformat them, rename them, or "modernize" them in place.
  New 64-bit code goes in a separate tree (see the plan). Preserve the copyright headers.
- `.ASM` files generated from `.C` (FLOPPY, FSYS, HARDIDE, JOBC, MONITOR, PARALLEL, RS232,
  UASM, and the CM32 lib) are build outputs. Edit the `.C` file, not the generated `.ASM`.
- Match surrounding style: tabs, upper-case mnemonics, `;` comments in ASM; K&R-style C.
- Windows host: shell is PowerShell, with Git Bash also available. Paths in old sources are DOS
  absolute (`c:\cm32\...`).
- Git: `.gitattributes` stores the original tree byte-for-byte (`-text`). New files under `tools/`, `port64/`
  and `*.md` use LF.
