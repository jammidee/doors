<!-- DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. -->
# DOORS System Manual

DOORS is a message-based, multitasking, real-time operating system for 64-bit x86 PCs. It is the
64-bit continuation of **MMURTL V1.0** by Richard A. Burgess. It keeps MMURTL's design, its system
call names, their parameter order and its error codes, and runs them in long mode with modern disks
and file systems.

This manual describes **DOORS 0.9.0 (x86-64)**, the system in `port64/`, during milestone M9.
The version history is in `CHANGELOG.md`.

| Chapter | Contents |
|---|---|
| [1. Introduction](01-introduction.md) | What DOORS is, its design ideas, what is finished and what is not |
| [2. Getting started](02-getting-started.md) | Requirements, building, running (`doors.bat`, `make`), the test disk |
| [3. The monitor](03-monitor.md) | The built-in command line and its commands |
| [4. System architecture](04-architecture.md) | Boot, memory, tasks and the scheduler, jobs, system calls |
| [5. Messaging](05-messaging.md) | Exchanges, messages, requests, services, timers |
| [6. File system](06-file-system.md) | Disks, volumes, paths, FAT and ext4, the file calls |
| [7. Writing programs](07-programming.md) | Your first program, the user library, writing a service |
| [8. System call reference](08-system-calls.md) | Every call by number, its parameters and its status |
| [9. Error codes](09-error-codes.md) | Every `Erc*` code and what it means |
| [10. Testing and tools](10-testing-and-tools.md) | `make test`, host-side disk checks, debugging |
| [11. Limits and known issues](11-limits.md) | Table sizes, unfinished parts, differences from MMURTL |

## Conventions in this manual

- `C:\BIN\HELLO.ELF` is a DOORS path. `port64/kernel/fsys.c` is a file in the source tree.
- Types are those of `port64/include/doors/types.h`: `U8`, `U16`, `U32` and `U64` are unsigned
  integers of that many bits, and `erc_t` is a 32-bit error code where `0` (`ErcOk`) means success.
- **MMURTL** means Burgess's original 32-bit system; **DOORS** means this system.
- A *tick* is 10 ms, the timer period, the same as in MMURTL.

## Related documents

- `PLAN.md`: milestones of the 64-bit port (M0–M10) and their acceptance tests.
- `docs/PORT64_PLAN.md`: the technical design of the port.
- `FEATURE.md` / `FEATURE_LAYMAN.md`: the feature list, technical and in plain language.
- `devlogs/`: the day-by-day development log.
- `PROBLEMS.TXT`: Burgess's list of known problems in MMURTL V1.0.
