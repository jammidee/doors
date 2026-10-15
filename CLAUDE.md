# CLAUDE.md

## What this is

MMURTL V1.0 — Richard A. Burgess's message-based, multitasking, real-time 32-bit x86
protected-mode OS from the book *Developing Your Own 32-bit Operating System* (1991–1999,
now public domain). Everything here is DOS-era: 8.3 upper-case filenames, `.BAT` builds,
tools that run under MS-DOS.

**Active goal:** port MMURTL to x86-64 (long mode). The plan is in
[docs/PORT64_PLAN.md](docs/PORT64_PLAN.md) — read it before doing any porting work and keep
its phase checklists up to date. Decisions already made:
- Keep every public API name, parameter order and `Erc*` code. Only types widen.
- Native file system is modern (ext4 subset) behind a VFS. FAT stays for interchange.
  The `MFILES.H` file API is kept, with sizes and offsets widened to 64-bit.

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
- Run: boot DOS without EMM386, run `MMSYS\MMLOADER.EXE`. Use DOSBox-X / QEMU / Bochs with a DOS
  image; `MSCODE16/BOOTCODE/MMURTL.IMG` is a bootable floppy image.
- There are no tests. Verification means booting it and exercising the CLI.

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
- Not yet a git repository. Run `git init` and commit the pristine tree before starting the port.
