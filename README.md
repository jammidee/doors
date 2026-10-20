<!-- DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. -->
# DOORS

**DOORS is a hobby project: a port of MMURTL, a small operating system from the 1990s, to
64-bit x86 PCs.**

MMURTL ("Message-based Multitasking Real-time Kernel") was written by Richard A. Burgess for his
book *Developing Your Own 32-bit Operating System*, and its source is now in the public domain.
It is a 32-bit protected-mode OS for DOS-era PCs. DOORS takes it to x86-64 long mode and keeps
its design:
- programs that talk to each other and to the system's services through messages;
- the same system calls, with the same names and error codes;
- its command line, its editor and its sample programs.

## A hobby, not a product

DOORS is something I build in my free time to learn, and because old systems like this are fun
to bring back to life. Please keep that in mind:

- **It isn't meant for real use.** Run it in an emulator (QEMU) or on a spare machine, never on a
  disk with data you care about.
- **There are no promises:** no support, no schedule, no warranty. It moves forward when I have
  time, sometimes a lot in a week, sometimes nothing for a while.
- **It is still a learning project.** The aim is to understand how an operating system works,
  from the boot code and memory management up to file systems, by porting a real one and testing
  it carefully.
- **The original stays untouched.** Burgess's MMURTL V1.0 source is in this repository unchanged,
  and the 32-bit system still boots, so every change can be compared with how MMURTL did it.

If you find it interesting, you're welcome to read the code, follow the devlogs or try it.

## Where it stands

Version 1.10.0: the 64-bit port does what MMURTL does, and passes MMURTL's own smoke test.
- **Kernel:** tasks and priorities, messages and services, separate address spaces for programs,
  ring-3 user mode.
- **Storage:** an ATA disk driver, FAT12/16/32 with long names, and ext4 with a journal, which
  survives power cuts. DOORS can boot from an ext4 system disk.
- **Programs:** MMURTL's CLI, its editor and its sample programs, running on 64-bit.
- **Tests:** a large self-test suite, power-cut tests, file-system fuzzing, static analysis and
  coverage measurement, all automated.

What comes next is in [ROADMAP.md](ROADMAP.md): real hardware, more processors, networking and
more.

## Trying it

In WSL or Linux (Ubuntu 22.04, with gcc, nasm, grub, xorriso and QEMU):

```
cd port64
make          # build the kernel and a bootable ISO
make console  # boot DOORS in QEMU, drawn in the terminal (doors.bat does this from Windows)
make test     # run the self-tests
```

Once DOORS is running, press **F1** at the monitor for MMURTL's CLI, and type `help`.

## Reading further

| | |
|---|---|
| [devman/](devman/README.md) | the DOORS System Manual: using it, its calls, error codes and limits |
| [PLAN.md](PLAN.md), [docs/PORT64_PLAN.md](docs/PORT64_PLAN.md) | how the port was planned, milestone by milestone |
| [ROADMAP.md](ROADMAP.md) | the way from here to a stable system |
| [CHANGELOG.md](CHANGELOG.md), [PROGRESS.md](PROGRESS.md) | what changed in each version, and when |
| [devlogs/](devlogs/) | the day-by-day notes: what was done, what broke, and why |
| `OSSOURCE/`, `MSAMPLES/`, `MMSYS/` | the original MMURTL V1.0 source, samples and system |
| `port64/` | DOORS 64-bit |

## Credits

- **MMURTL V1.0:** Richard A. Burgess, public domain. The book, with the code and its updates, is
  free at http://www.ipdatacorp.com/mmurtl/.
- **DOORS** (the 64-bit port and everything under `port64/`, `tools/` and the documents above):
  Copyright (c) 2026 Joel Damaso, Philippines.

---

## The original README

This repository started from a copy of the MMURTL source that someone else had put online. Its
README read:

> This little operating system was written by Richard A. Burgess in the early 90s and written about in the book "Developing your own 32-bit operating system".  The code has since been placed in the public domain.
>
> I bought this book in 1996 while I was still in high school and it was one of the most influencial programming books I've read.  And still is.  I learned a ton from this book and from this code and spent the next three years learning about operating systems and compilers.
>
> For anyone interested, here is the full source code.
>
> The book is available for free in PDF format, along with the code and updates (all of which have been merged into this repo).
> http://www.ipdatacorp.com/mmurtl/
