# DOORS — what it does, in plain words

DOORS is a small operating system: the basic software that runs a computer and lets programs share it.
Windows, macOS and Linux are operating systems too. DOORS is much smaller and simpler, which makes it a
good system to learn from and to rebuild in a modern way.

It is based on **MMURTL**, an operating system written in the 1990s by Richard Burgess for his book about
building your own operating system. We took that system, got it running again, and are now rebuilding it for
today's computers under the new name **DOORS**.

*Last updated: 16 October 2026*

---

## What works today

These have all been tried and checked on a virtual computer (an emulator called QEMU, which runs inside
Windows). A test script repeats the checks automatically: 20 checks pass, and the 3 known problems below are
tracked.

### It starts up
- DOORS starts from a virtual floppy disk, as many computers did in the 1990s.
- At startup it finds the memory, the keyboard, the disk drives, the serial port (for modems and terminals) and the printer port.
- It reads both a **floppy disk** (drive A:) and a **hard disk** (drive C:).

### It runs several programs at once
- You can run **several programs at the same time**, each with its own screen.
- **Ctrl + Alt + Page Down** flips between them, like Alt-Tab on Windows.
- One program can't freeze the others. A test program that does nothing but spin in a loop still lets
  everything else keep working.
- **Ctrl + Alt + Delete** closes the program on screen and gives back its memory.

### It has a command line
A text window where you type commands (like the Windows Command Prompt):

| You type | It does |
|---|---|
| `dir` | Lists the files in a folder |
| `type` | Shows what's inside a text file |
| `copy`, `ren`, `del` | Copies, renames and deletes files |
| `md` | Makes a new folder |
| `path` | Moves to another folder |
| `dump` | Shows a file's raw contents, byte by byte |
| `help` | Lists all the commands |

### It comes with programs
- **A text editor:** create a file, type in it, save it, and open it again later.
- **A system monitor:** shows which programs are running, how much memory is free, and how busy the
  computer is.
- **A built-in debugger:** a tool for programmers that shows what the processor is doing, step by step.
- **Sample programs**, including a small "number service" that other programs can ask for numbers.
  This shows the heart of DOORS: **programs talk to each other by sending messages**, like passing notes.

---

## Known problems (inherited from the 1990s original)

| Problem | What happens |
|---|---|
| Copying a file can lose its end | The copy looks the right size, but the last part can be missing |
| Removing a folder freezes the computer | The `rd` command was never finished in the original |
| File dates are wrong | New files get a nonsense date |
| Long file names in the editor | If the full name is longer than 13 characters, the end gets cut off |
| `undel` (undelete) is missing | It's listed in the help but doesn't exist |

These are written down so the new version can fix them.

---

## What's coming: DOORS for modern computers

The original only runs on old 32-bit processors (the kind from the 1990s). We are rebuilding it to run
natively on today's **64-bit** processors. The plan:

- **Starts on modern PCs** without needing old DOS software first.
- **Uses all the memory of a modern computer**, not just the 4 GB limit of old systems.
- **A modern file system (ext4, the one Linux uses):**
  - long file names (not just 8 letters + 3),
  - files bigger than 4 GB,
  - protection against losing data if the power goes off.
  It can still read and write the old floppy-style (FAT) disks to exchange files.
- **Safer programs:** a program that misbehaves gets an error message instead of crashing the whole
  computer.
- **The same commands and programs**, so everything above keeps working, now with the known problems fixed.

We'll build it in steps. Each step must start up and work before moving on:
1. ✅ Get the original running and test everything (done)
2. ✅ Start up on a modern 64-bit processor (done: DOORS now boots in 64-bit mode and shows its banner)
3. ✅ Keyboard, clock and screen (done: the 64-bit DOORS keeps time 100 times a second, reads the keyboard, and shows a clear error report if something goes wrong)
4. ✅ Memory management (done: DOORS hands out memory safely, keeps each program's memory private, and catches runaway programs before they damage anything)
5. ✅ Running several programs at once (done: the 64-bit DOORS shares the processor fairly between tasks and lets urgent ones go first)
6. Programs sending messages to each other
7. Disks, then the new file system
8. The command line, editor and other programs, then re-check that everything above still works

*Progress is logged day by day in the `devlogs` folder. The detailed technical plan is in `PLAN.md`.*
