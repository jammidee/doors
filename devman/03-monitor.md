<!-- DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. -->
# 3. The monitor

Until the MMURTL CLI is ported (milestone M9), DOORS has a small command line built into the
kernel (`port64/kernel/main.c`). It runs as a kernel task at priority 20 and uses job 1's
screen. Its top line shows the version, the clock and the tick counter, as MMURTL's Monitor did.
Type a command and press **Enter**. **Backspace** corrects the line.

## Keys

| Key | What it does |
|---|---|
| **F1** | load the CLI (`run cli`) |
| **F2** | list the jobs (`jobs`) |
| **F3** | tasks and resource statistics (`tasks`) |
| **Ctrl-Alt-PgDn** | show the next job's screen. Keys go to the job on screen. Press it again to get back to the monitor |
| **Ctrl-Alt-Del** | kill the job on screen (the monitor itself can't be killed) |

Every program has its own screen. A program started with `run` draws on its own screen, so its
output doesn't appear on the monitor's. Use Ctrl-Alt-PgDn or `screen N` to look at it. The serial
log (`build/serial-run.log` with `doors.bat`) gets every program's text.

## Commands

| Command | What it does |
|---|---|
| `help` | lists the commands |
| `ver` | the DOORS version and the git commit it was built from |
| `ticks` | the 10 ms tick counter and the time since interrupts were enabled |
| `mem` | free memory (`QueryPages`), then allocates and frees 4 OS pages as a check |
| `tasks` | every task (number, job, priority, state, CPU ticks, name), scheduler statistics, and how many exchanges, link blocks, request blocks, timers and services are free or in use |
| `spin` | starts two busy tasks at priority 25 for 3 seconds. Watch them share the CPU with `tasks` |
| `run NAME` | loads boot module `NAME` (for example `run hello`) as a new job |
| `run C:\PATH\FILE.ELF` | loads a program from a file on any volume (for example `run F:\BIN\HELLO.ELF`) |
| `jobs` | every job (number, name, running or ended with its error code, task count) and the list of boot modules |
| `kill N` | ends job `N` (`KillJob`) |
| `screen N` | shows job `N`'s screen (`SetVidOwner`) |
| `vols` | the volumes: letter, disk, partition, size, type and label |
| `dir C:\PATH` | lists a directory on any volume (FAT or ext4): date and time, `<DIR>`, size, name |
| `type C:\FILE` | prints a file |
| `int3` | raises a breakpoint exception. It is handled and the monitor continues |
| `pf` | writes to the unmapped address `0xdead0000`: page fault, register dump, kernel panic |
| `div0` | divides by zero: register dump, kernel panic |

`pf` and `div0` stop the system on purpose. They show what a kernel fault report looks like.
A fault in a **user** program does not stop the system: the job is ended with error
`1000 + vector` (for example `1014` for a page fault) and DOORS continues.

## Examples

```
> vols
C:  disk 12 part 1     40 MB  FAT32  DOORS32
D:  disk 12 part 2     16 MB  FAT16  DOORS16
E:  disk 12 part 3      4 MB  FAT12  DOORS12
F:  disk 12 part 4     64 MB  ext4   DOORS-EXT4
> dir F:\
2026-10-15 18:58  <DIR>        16384  lost+found
2026-10-15 18:58                  21  A long file name on ext4.txt
2026-10-15 18:58              300000  BIG.BIN
2026-10-15 18:58  <DIR>         4096  BIN
2026-10-15 18:58                  17  HELLO.TXT
...
> type F:\HELLO.TXT
Hello from ext4.
> run F:\BIN\HELLO.ELF
Loaded F:\BIN\HELLO.ELF as job 4
hello: running in ring 3 as job 4 (task exchange 7)
...
Job number 4 terminated. Error: 0
```

## Notes

- Paths always start with a drive letter. FAT names ignore case (`dir C:\dir1` works).
  ext4 names are case-sensitive (`F:\MixedCase.Txt` and `F:\mixedcase.txt` are different).
- Times on ext4 are shown in UTC, because ext4 stores Unix time and QEMU's clock runs in UTC.
  FAT stores local time with no time zone, and it is shown as written.
- The monitor is a development tool. Its commands call kernel functions directly and are not a
  stable interface. Programs use the system calls of [chapter 8](08-system-calls.md).
