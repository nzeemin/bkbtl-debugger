# bkbtl-debugger

A console (command-line) debugger for [BKBTL](https://github.com/nzeemin/bkbtl), an emulator
of the Soviet BK-0010/BK-0011M home computer line. It links against BKBTL's `emubase` emulation
core directly, with no GUI dependency, so it builds and runs anywhere a C++17 compiler does —
Linux, macOS, and Windows.

The command set is a console port of the table-driven command dispatcher from BKBTL's WinAPI
GUI debugger (`emulator/ConsoleView.cpp`), reworked for a plain stdin/stdout session, plus a number of commands and conveniences that only make sense in a scriptable,
non-interactive-capable tool: configuration selection from the command line, save/load state,
floppy image attach/detach, screenshot export, and instruction tracing to a log file.

## Why

The GUI debugger is great for interactive work, but it's Windows-only and not scriptable. This
tool exists for the cases where you want to:

- build and run the emulator in a CI pipeline, a container, or any non-Windows environment,
- drive a debugging or reverse-engineering session from a script (feed it commands over stdin,
  capture state, compare runs),
- automate things like "boot this disk image for N frames and check what's on screen" without a
  display.

## Building

Requires a C++17 compiler (tested with GCC and Clang on Linux and macOS; MSVC/Visual Studio 2022
project files are also included for Windows).

```sh
make            # release build (default) -> build/release/bkbtldebug
make debug      # debug build -> build/debug/bkbtldebug
make run        # build (release) and run
make run-debug  # build (debug) and run
make clean      # remove build artifacts
```

On Windows, open `bkbtldebug.sln` in Visual Studio 2022.

### ROM files

The emulator needs the original BK monitor/BASIC/FOCAL ROM dumps to boot. These are copyrighted
firmware and are **not included** in this repository. Obtain them yourself (e.g. from your own
hardware, or wherever you already source BKBTL's ROM set) and copy the `.rom` files into the
directory you run the debugger from:

```
monit10.rom  basic10_1.rom  basic10_2.rom  basic10_3.rom   (BK-0010 + BASIC)
monit10.rom  focal.rom                                      (BK-0010 + FOCAL)
b11m_bos.rom  b11m_ext.rom  b11m_mstd.rom  basic11m_0.rom  basic11m_1.rom   (BK-0011M)
disk_253.rom  disk_326.rom  disk_327.rom                    (floppy controller ROMs, FDD configs)
```

If a ROM is missing, the debugger reports which one and exits cleanly rather than crashing.

## Running

```sh
./bkbtldebug                          # default configuration: BK-0010-BASIC
./bkbtldebug conf:BK-0011M-FDD        # pick a configuration explicitly
./bkbtldebug --romdir /path/to/roms   # look for .rom files there instead of the current directory
```

`--romdir DIR` takes a separate argument (not `--romdir=DIR`) and can be combined with `conf:NAME`
in either order. If a ROM still can't be found, the debugger reports the exact path it tried.

Available configuration names (`conf:NAME`, case-insensitive):

| Name | Machine |
|---|---|
| `BK-0010-BASIC` (default) | BK-0010(01) with BASIC |
| `BK-0010-FOCAL` | BK-0010(01) with FOCAL |
| `BK-0010-FDD` | BK-0010(01) with floppy controller |
| `BK-0011M` | BK-0011M, no floppy controller |
| `BK-0011M-FDD` | BK-0011M with floppy controller |

On startup the debugger prints the selected configuration and drops into a command prompt
showing the current PC in octal, e.g. `100000>`.

### Example: boot RT-11 from a floppy image

```
$ ./bkbtldebug conf:BK-0011M-FDD
BKBTL emulator console debugger [...]
Configuration: BK-0011M-FDD
Use 'h' command to show help.
140000> diskA attach rt11.img
Attached diskA: rt11.img
140000> continue frames 750
 Stopped at 162646
162646> i floppy
Floppy engine: ON
  diskA: attached, read-write (selected)
  diskB: not attached
  diskC: not attached
  diskD: not attached
177130 040202 floppy state
177132 000400 floppy data
       000006 track
       000001 side
162646> screenc boot.png
Saved screenshot boot.png
```

`boot.png` will show the RT-11-derived OS booting, with the floppy drive light effectively "on"
during the read bursts and idle once the system reaches its prompt.

## Commands

Single-letter commands never take a space before their argument (`d100260`, `r0=123`); full-word
commands always do (`disasm 100260`, `continue frames 10`). Numeric arguments are octal
throughout, **except** the frame count in `continue frames N`, which is decimal.

Commands that change state (registers, breakpoints, reset, file/disk operations) print a short
confirmation line. `disasm`/`d`/`D` and `examine`/`x` page their output 8 lines at a time: after
a page, the prompt becomes `-- more (Enter to continue) --`; pressing Enter shows the next page
at the same address/format/modifiers, and any other input just cancels paging (that input is
discarded, not run as a command) and returns to the normal prompt.

### Help

| Command | Description |
|---|---|
| `h`, `help`, `?` | Show the command list |
| `q`, `quit`, `exit` | Quit the debugger |

### Execution control

| Command | Description |
|---|---|
| `reset` | Reset the machine; confirms with `Reset.` |
| `c`, `continue` | Continue; free run (gives up after 3000 frames if no breakpoint is hit) |
| `cXXXXXX`, `continue XXXXXX` | Continue; run and stop at address `XXXXXX` |
| `continue frames N` | Continue; run for exactly `N` frames (decimal; 25 frames = 1 second) |
| `s`, `step` | Step Into; execute one instruction |
| `n`, `next` | Step Over; execute the current instruction and stop right after it (steps over `CALL`/`JSR`) |

### Registers

| Command | Description |
|---|---|
| `r`, `regs` | Show all registers and PSW flags, one compact line |
| `r ext`, `regs ext` | Show extended (I/O port) registers: keyboard, palette, scroll, timer, parallel port, system register |
| `i floppy`, `info floppy` | Show floppy engine state, per-drive attach status, and floppy controller registers/track/side |
| `rN` | Show register `N` (0-5), octal + binary |
| `rN=XXXXXX` | Set register `N` to `XXXXXX`; confirms with `RN set to XXXXXX` |
| `rps` / `rps=XXXXXX` | Show / set the processor status word |
| `rpc` / `rpc=XXXXXX` | Show / set PC |
| `rsp` / `rsp=XXXXXX` | Show / set SP |

### Disassembly and memory

| Command | Description |
|---|---|
| `d`, `disasm` | Disassemble from PC (paged) |
| `D` | Disassemble from PC, short format (no raw opcode words) |
| `dXXXXXX`, `disasm XXXXXX` | Disassemble from address `XXXXXX` |
| `x`, `examine` | Examine memory at PC (paged) |
| `xXXXXX`, `examine XXXXXX` | Examine memory at address `XXXXXX` |
| `... bytes` | Modifier: byte granularity instead of words |
| `... hex` | Modifier: hexadecimal (uppercase `A`-`F`) instead of octal |
| `... nochars` | Modifier: hide the trailing ASCII/character column |

Modifiers go after the address, in any order, e.g. `x100260 bytes hex` or `examine hex nochars`.
Values that changed since the last step/run are highlighted (in a terminal that supports color;
suppressed automatically otherwise).

### Breakpoints

| Command | Description |
|---|---|
| `b` | List all breakpoints |
| `bXXXXXX` | Set a breakpoint at `XXXXXX`; confirms with `Breakpoint set at XXXXXX` |
| `bc` | Remove all breakpoints; confirms with `All breakpoints removed.` |
| `bcXXXXXX` | Remove the breakpoint at `XXXXXX`; confirms with `Breakpoint removed at XXXXXX` |

### Status and tracing

| Command | Description |
|---|---|
| `i`, `info` | Show uptime and (if the configuration has a floppy controller) drive status |
| `t`, `trace` | Toggle instruction tracing to `trace.log` on/off |
| `tXXXXXX`, `trace XXXXXX` | Set the trace mask explicitly (see `TRACE_xxx` in `emubase/Board.h`) |
| `tc`, `t clear`, `trace clear` | Clear `trace.log` |
| `mo`, `monitor` | Type `M` `O` `Enter` (BASIC) or `P` `SPACE` `M` `Enter` (FOCAL) to exit to the monitor |

Tracing only captures execution driven by `continue`/`c` (and the run-to-breakpoint path of
`next`/`n`); single `step`/`s` does not go through the same code path and produces no trace
output.

### Files and floppy images

| Command | Description |
|---|---|
| `memsave [FILE]` | Save a full 64K memory dump (default `memdump.bin`) |
| `loadbin FILE` | Load a classic BK `.bin` tape image (2-word header: start address, byte count) into RAM |
| `statesave FILE` / `stateload FILE` | Save / load full emulator state — memory, registers, ports |
| `diskN attach FILE`, `diskN a FILE` | Attach a floppy image to drive `N` (`A`-`D`) |
| `diskN detach`, `diskN d` | Detach the floppy image from drive `N` |
| `screen [FILE]` | Save a black-and-white screenshot as PNG (default filename: timestamp) |
| `screenc [FILE]` | Save a color screenshot as PNG |

## Platform notes

- Builds and has been tested on Linux (GCC, Clang) and macOS (Clang); MSVC/Visual Studio 2022
  project files are included but less continuously exercised.
- Console color output is automatically suppressed when stdout is not a terminal (e.g.
  redirected to a file or piped), so scripted/batch use never sees raw escape codes.
- `TCHAR` is `char` on non-Windows builds; the Win32 `wmain`/wide-string code paths only compile
  under MSVC.

## Status

This is a work in progress, developed interactively.
