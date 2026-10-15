# DOORS Smoke Test

The acceptance checklist for M0 (32-bit baseline, original MMURTL V1.0 binaries) and, later, M9 (64-bit DOORS).
Every row is run on both. The 64-bit result must match the 32-bit "expected" column, or fix the ✗ rows.

**How to run (32-bit):** in WSL, `bash tools/boot32/run.sh`. Drive it from Git Bash with
`MSYS_NO_PATHCONV=1 wsl -d Ubuntu-22.04 --cd <repo> -- python3 tools/boot32/qscreen.py --type '<cmd>' --enter --wait 2`.
Use **upper-case paths** and **no trailing backslash** on directories.

Legend: ✓ works · ⚠ works with a defect · ✗ broken · — not yet tested

## Boot and Monitor

| # | Check | Expected | 32-bit (2026-10-16) | 64-bit |
|---|---|---|---|---|
| B1 | Boot from floppy | Monitor screen, clock, tick counter | ✓ | — |
| B2 | Memory detection | Total 16384 KB / Free 15736 KB (QEMU `-m 16`) | ✓ | — |
| B3 | Driver init | KBD, serial, LPT, floppy, HDD: `Error: 0` | ✓ | — |
| B4 | File system init | `A: ... FAT12`, `C: Heads 16, Sec/Trk 17 ... FAT16`, `File System... Error: 0` | ✓ | — |
| B5 | F1 LdCLI | `Loading: A:\MMSYS\CLI.RUN... New CLI Job Number is: 3`, CLI screen | ✓ | — |
| B6 | Monitor Stats / Jobs keys | Resource stats, job list | — | — |
| B7 | Ctrl-Alt-PgDn | Switch screen and keyboard to the next job | — (system was hung, see F7) | — |

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
| F8 | `path C:\MSAMPLES\CLI` then `dir` | Path changed | — | — |
| F9 | Lower-case paths (`dir c:\msamples`) | Same as upper case | ✗ no case folding (`File doesn't exist` / Erc 224) | — |
| F10 | `dump`, `undel` | Hex dump / undelete | — | — |

## CLI — programs and jobs

| # | Check | Expected | 32-bit | 64-bit |
|---|---|---|---|---|
| P1 | `help` | Contents of `HELP.CLI` | — | — |
| P2 | `edit <file>` (`COMMANDS.CLI` → `C:\MSamples\Editor\Edit.run`) | Editor opens, saves, exits | — | — |
| P3 | `run C:\MSAMPLES\BUSYLOOP\BUSYLOOP.RUN` | Runs as a job, CLI still responsive | — | — |
| P4 | `Service` + `TestSvc` | Sample service answers requests | — | — |
| P5 | Second CLI (F1 again) + Ctrl-Alt-PgDn | Two CLIs, switch between them | — | — |
| P6 | Ctrl-Alt-Del | Kills the job on screen | — | — |
| P7 | `debug` / Monitor Debug | Debugger screen, F-keys work, F4 returns | — | — |
| P8 | `monitor`, `exit` | Back to Monitor / CLI job ends | — | — |

## Bugs found (32-bit baseline)

| Bug | Where | Notes for the 64-bit port |
|---|---|---|
| FAT timestamps are garbage | `FSYS.C` `GetFATTime()` is empty | Implement it from the CMOS RTC |
| `copy` truncates the last partial block (Erc 217) | CLI copy / `WriteBlock` past EOF | Byte-accurate copy; add a regression test |
| `rd` hangs the system | `DeleteDir` unfinished | Implement it (PLAN M7/M8) |
| No case-insensitive names | FSYS name matching | Fold case for FAT 8.3; ext4 is case-sensitive by design |
| Head count taken from partition `HeadEnd` | `FSYS.C:579` (binary lacks the CMOS override) | Use LBA and IDENTIFY, never CHS from the MBR |
