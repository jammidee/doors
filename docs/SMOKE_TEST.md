# DOORS Smoke Test

The acceptance checklist for M0 (32-bit baseline, original MMURTL V1.0 binaries) and, later, M9 (64-bit DOORS).
Every row is run on both. The 64-bit result must match the 32-bit "expected" column, or fix the ✗ rows.

**How to run (32-bit):** in WSL, `bash tools/boot32/run.sh`. Drive it from Git Bash with
`MSYS_NO_PATHCONV=1 wsl -d Ubuntu-22.04 --cd <repo> -- python3 tools/boot32/qscreen.py --type '<cmd>' --enter --wait 2`.
Use **no trailing backslash** on directories (except for `path`).

**Scripted:** `python3 tools/boot32/smoke.py [--only P2,P4]` (in WSL) runs every row below except F7 on a fresh, headless VM.
Known-bug rows are XFAIL. Last result (2026-10-16): **PASS 20, FAIL 0, XFAIL 3** (F5, F10b, P2b), stable over repeated runs.

Legend: ✓ works · ⚠ works with a defect · ✗ broken · — not yet tested

## Boot and Monitor

| # | Check | Expected | 32-bit (2026-10-16) | 64-bit |
|---|---|---|---|---|
| B1 | Boot from floppy | Monitor screen, clock, tick counter | ✓ | — |
| B2 | Memory detection | Total 16384 KB / Free 15736 KB (QEMU `-m 16`) | ✓ | — |
| B3 | Driver init | KBD, serial, LPT, floppy, HDD: `Error: 0` | ✓ | — |
| B4 | File system init | `A: ... FAT12`, `C: Heads 16, Sec/Trk 17 ... FAT16`, `File System... Error: 0` | ✓ | — |
| B5 | F1 LdCLI | `Loading: A:\MMSYS\CLI.RUN... New CLI Job Number is: 3`, CLI screen | ✓ | — |
| B6 | Monitor F2 Jobs / F3 Stats | Job list / resource stats | ✓ Jobs lists Monitor, Debugger, CLIs; Stats shows pages, task switches (incl. preemptive), idle ticks, free TSS/JCB/RqBlk/LB/Exch | — |
| B7 | Ctrl-Alt-PgDn | Switch screen and keyboard to the next job | ✓ cycles Monitor → CLI 3 → CLI 4 → Monitor | — |

## CLI — file commands

| # | Command | Expected | 32-bit (2026-10-16) | 64-bit |
|---|---|---|---|---|
| F1 | `dir` | Lists current path (A:\MMSYS) | ✓ | — |
| F2 | `dir C:\` / `dir C:\MSAMPLES` | Root (label MMURTL, MMSYS, MSAMPLES) / sample dirs, paged | ✓ | — |
| F3 | `type C:\MSAMPLES\READ.ME` | File text, then `Error: 1` (EOF) / `End of file` | ✓ | — |
| F4 | `md C:\TEST` | Directory created | ⚠ timestamp shown as `06-15-54` (`GetFATTime` is an empty stub) | — |
| F5 | `copy C:\MSAMPLES\READ.ME C:\TEST\READ.ME` | Identical copy | ⚠ size is right (627) but content is cut at ~512 bytes. `Error 00217` (`ErcBeyondEOF`): the last partial block isn't written | — |
| F6 | `ren C:\TEST\READ.ME C:\TEST\NOTE.TXT` | Renamed | ✓ | — |
| F6b | `del C:\TEST\NOTE.TXT` | `Done.`, file gone | ✓ | — |
| F7 | `rd C:\TEST 0` | Empty dir removed | ✗ **hangs the whole system** (CPU loops at EIP `0x00022C21`, ring 0, IF=1; keyboard and clock dead). `DeleteDir` was never finished (`PROBLEMS.TXT` 2a). **Don't run it in automated tests.** | — |
| F8 | `path C:\MSAMPLES\CLI\` then `dir` | Path changed, dir lists it | ✓ (path needs the trailing backslash; `dir` doesn't) | — |
| F9 | Lower-case paths (`dir c:\msamples`) | Same as upper case | ✓ (the earlier failure was caused by the C: geometry bug, not case handling) | — |
| F10 | `dump C:\HELLO.TXT` / `undel` | Hex dump / undelete | ✓ dump · ✗ `undel` listed in `HELP.CLI` but CLI says `Command not found` | — |

## CLI — programs and jobs

| # | Check | Expected | 32-bit | 64-bit |
|---|---|---|---|---|
| P1 | `help` | Contents of `HELP.CLI` (paged) | ✓ | — |
| P2 | `edit C:\HELLO.TXT` | Create? Y, type, Alt-S save, Alt-Q quit; `type` shows text | ✓ (17 bytes saved and read back) | — |
| P2b | `edit C:\SMOKE\HELLO.TXT` (path > 13 chars) | Saved as `HELLO.TXT` | ✗ the name is cut to 13 chars (`C:\SMOKE\HELLO.` → file `HELLO`, no extension) | — |
| P3 | `run C:\MSAMPLES\BUSYLOOP\BUSYLOOP.RUN` | Prints the timer tick forever, rest of system still responsive | ✓ ticks advance ~100/s (10 ms); preemption works | — |
| P4 | `Service` in CLI 3, `TestSvc` in CLI 4 | NUMBERS service hands out 0, 1, …; any key stops the service | ✓ | — |
| P5 | Second CLI (F1 again) + Ctrl-Alt-PgDn | Two CLIs, switch between them | ✓ jobs 3 and 4 | — |
| P6 | Ctrl-Alt-Del on BUSYLOOP | Kills the job on screen | ✓ `Job number 3 terminated. Error: 4`; free memory back to 15736 KB | — |
| P7 | CLI `debug` | Registers + disassembly, F6 task list, **ESC** exits (F4 only re-shows CS:EIP) | ✓ | — |
| P8 | `monitor`, `exit` | Back to Monitor / CLI job ends | ✓ job gone from F2 list | — |

## Bugs found (32-bit baseline)

| Bug | Where | Notes for the 64-bit port |
|---|---|---|
| FAT timestamps are garbage | `FSYS.C` `GetFATTime()` is empty | Implement it from the CMOS RTC |
| `copy` truncates the last partial block (Erc 217) | CLI copy / `WriteBlock` past EOF | Byte-accurate copy; add a regression test |
| `rd` hangs the system | `DeleteDir` unfinished | Implement it (PLAN M7/M8) |
| Editor keeps only 13 chars of the file name | EDIT.RUN, or the command line passed to it (`Filename[60]` itself is big enough) | Long names are a DOORS goal; test with long paths |
| `undel` advertised but missing | `HELP.CLI` vs `CLI.C` | Implement or drop from help |
| Head count taken from partition `HeadEnd` | `FSYS.C:579` (binary lacks the CMOS override) | Use LBA and IDENTIFY, never CHS from the MBR |

## Cosmetic

- Something writes `11` at the start of screen row 2 on every job's screen (seen as `11URTL` on the Monitor).
- `dump` shows `0x0A` as `.e` in the ASCII column.
