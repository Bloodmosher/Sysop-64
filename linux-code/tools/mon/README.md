# mon — C64 Hardware Monitor

An interactive machine-language monitor for the Commodore 64, running on the ARM core of a DE-10 Nano. It communicates with the C64 over the Sysop-64 hardware bridge, giving you memory inspection and editing, a disassembler, an assembler, breakpoints, single-stepping, register display, file loading/saving, and bus-level DMA stepping — all from a terminal on the host.

Project home: https://github.com/Bloodmosher/Sysop-64

---

## How it works

`mon` uses `libsysop64` to take ownership of the C64 expansion bus (the "DMA" mechanism), then reads and writes C64 memory by injecting bytes via the FPGA's DMA write queue. Most commands follow the pattern:

1. Acquire bus (`OwnDma` RAII guard calls `dma_enable()`).
2. Read or write C64 RAM using `peek()` / `poke()`.
3. Release bus when the guard goes out of scope (`dma_disable()`).

Breakpoints and single-stepping also rely on a small 6502 stub (`mon_io.asm`) that is injected into C64 RAM at `$DF00`. When the C64 CPU hits a `BRK` instruction the stub saves the CPU state to the stack and loops reading a command byte from `$DFF1`, allowing `mon` to inspect and modify registers before resuming.

---

## Architecture diagram

```
 Terminal (stdin/stdout)
        │
        ▼
 ┌──────────────────────────────────────┐
 │  mon.cpp  — command dispatch loop    │
 │  (parse line → call handler)         │
 ├──────────────┬───────────────────────┤
 │ mon_input    │  mon_breakpoints      │
 │ (line editor │  (BRK table, mirror   │
 │  + history)  │   to C64 RAM)         │
 ├──────────────┼───────────────────────┤
 │  mon_cpu     │  mon_dma              │
 │ (registers,  │  (sampler-based step/ │
 │  step, save) │   break/resume)       │
 ├──────────────┴───────────────────────┤
 │  instructions.cpp / asm.cpp          │
 │  (6502 opcode table, assembler)      │
 └──────────────────────────────────────┘
        │
        ▼
 libsysop64  (poke / peek / dma_enable / sampler …)
        │
        ▼
 FPGA bridge → C64 expansion bus
        │
        ▼
 mon_io.asm stub in C64 RAM ($DF00)
```

---

## Source files

### `mon.cpp`
Main entry point and command dispatch loop.

- Initialises `libsysop64` (`sysop_init()`), sets up the raw terminal, and installs a `SIGINT` handler.
- Reads one line at a time via `get_line()`, tokenises it, and dispatches to the appropriate handler.
- Contains the implementations of most commands (memory dump, disassemble, assemble, hunt, fill, charset dump, VIC info, load, cartridge commands, etc.).
- Defines the three globals shared across all modules: `g_pc`, `g_status`, `own_dma`.

### `mon_private.h`
Shared internal header included by every `.cpp` file.

- System and library includes.
- Constants: `MAX_BUFFER_SIZE`, `MAX_ARGUMENTS`, `MAX_LINE_LENGTH`, `HISTORY_MAX_LINES`.
- C64 debug protocol addresses: `STORED_STACK_ADDRESS` (`$DFF0`), `BREAKPOINT_COUNT_ADDRESS` (`$DFE0`), `BREAKPOINT_TABLE_ADDRESS` (`$DFE1`), `MAX_BREAKPOINTS` (5).
- Extern declarations for the three shared globals.
- **`OwnDma`** — RAII C++ class. On construction: if `own_dma == 0`, calls `dma_enable()` and sets `own_dma = 1`. On destruction: if it acquired DMA, calls `dma_disable()` and clears `own_dma`. Safe to nest — only the outermost instance toggles the hardware.

### `mon_input.cpp` / `mon_input.h`
Line editor and terminal management.

- `setup_raw_terminal()` / `reset_term()`: Put stdin into raw non-echo mode; restore on exit or `SIGINT`.
- `get_line(buf, len)`: Reads one line with arrow-key command history (ring buffer of 100 entries, 50 chars each) and backspace editing. Returns `NULL` on empty input.
- `sigintHandler()`: Restores the terminal and exits cleanly.

### `mon_breakpoints.cpp` / `mon_breakpoints.h`
Breakpoint management. Breakpoints are mirrored into C64 RAM so the on-C64 stub can find them.

**Data**: `g_breakpoints[MAX_BREAKPOINTS]` — array of `{ uint16_t address; uint8_t opcode }` structs. `g_num_breakpoints` counts active entries.

**Protocol**: `$DFE0` holds the count; `$DFE1` holds the table (5 × 3 bytes: addr_lo, addr_hi, original_opcode).

**Key functions**:
| Function | Description |
|----------|-------------|
| `set_breakpoint(addr)` | Saves the original opcode, pokes `BRK` ($00) at `addr`, updates the C64-side table |
| `remove_breakpoint(addr)` | Restores the original opcode, removes the entry |
| `clear_breakpoints(restore_opcodes)` | Optionally restores all opcodes, clears the table |
| `read_breakpoints()` / `write_breakpoints()` | Sync the in-memory table from/to C64 RAM |
| `refresh_breakpoints_if_needed(force)` | Reads from C64 RAM if `g_breakpoint_refresh_needed` is set |
| `is_breakpoint(addr)` | Returns the saved opcode (or -1) for `addr` |

### `mon_cpu.cpp` / `mon_cpu.h`
CPU state, register display, single-stepping, and file I/O.

**Register access protocol**: When the C64 stub hits a `BRK` and enters its loop, the CPU state is preserved on the hardware stack in a fixed layout. `show_registers()` reads `$DFF0` to get the stack pointer, then reads above it: Y (+1), X (+2), A (+3), SR (+4), PC-lo (+5), PC-hi (+6). If the `B` flag (bit 4) is set in SR, subtracts 2 from the return address to account for the `BRK` instruction length.

| Function | Description |
|----------|-------------|
| `show_registers()` | Print PC, SR, A, X, Y, SP; updates `g_pc` and `g_status` |
| `setpc(addr)` | Patch the return address on the C64 stack to change where the CPU resumes |
| `resume()` | Restore the breakpoint's original opcode, patch PC, re-arm the BRK vector if other breakpoints remain, signal the stub to exit |
| `determine_next_pc(pNextPc)` | Decode the instruction at `g_pc` and compute the PC value after it executes (handles all addressing modes, branches, jumps) |
| `run()` | Inject `RUN\r` into the C64 keyboard buffer |
| `save(start, end, filename, prg)` | Dump C64 memory range to a file; optionally prepend a 2-byte load-address header |

### `mon_dma.cpp` / `mon_dma.h`
DMA/sampler-based CPU control. These commands do not rely on the on-C64 stub; instead they use the Sysop-64 bus sampler to observe bus cycles while toggling the `_DMA` line.

| Function | Description |
|----------|-------------|
| `command_dma_break()` | Assert DMA, run the sampler, capture bus cycles until the CPU is seen to have stopped, set `own_dma = 1` |
| `command_dma_step()` | If not frozen: break. If frozen: momentarily release then re-assert DMA to advance one CPU cycle, print the bus state |
| `command_dma_resume()` | De-assert DMA, capture post-release samples, set `own_dma = 0` |
| `wait_for_command_finish()` | Spin until the C64-side stub clears `$DFF1` (signals it has processed the current command) |

### `mon_io.asm`
6502 assembly stub that runs on the C64 at `$DF00`. It is injected into C64 RAM by `mon` whenever the NMI freeze path is used. The stub:

1. Saves all registers (A, X, Y) and the stack pointer to `$DFF0`–`$DFF4`.
2. Loops reading a command byte from `$DFF1`:
   - `$01` — write `$DFF2` to `$01` (CPU port), store result in `$DFF4`
   - `$02` — set stack pointer from `$DFF2` (via `TXS`)
   - `$03` — read `$01`, store in `$DFF4`
   - `$FF` — restore registers and `RTI` (resume)
3. Data section at `$DFE0`: `num_breakpoints` byte, then the breakpoint table (5 × 3-byte entries).

The shared memory map between `mon` (ARM) and the stub (6502):

| Address | Symbol | Direction | Description |
|---------|--------|-----------|-------------|
| `$DFF0` | `STORED_STACK_ADDRESS` | C64→ARM | Stack pointer value saved on NMI entry |
| `$DFF1` | — | ARM→C64 | Command byte (ARM writes; stub clears to $00 when done) |
| `$DFF2` | — | ARM→C64 | Command parameter byte |
| `$DFF3` | — | ARM→C64 | Scratch |
| `$DFF4` | — | C64→ARM | Result byte |
| `$DFE0` | `BREAKPOINT_COUNT_ADDRESS` | ARM↔C64 | Number of active breakpoints |
| `$DFE1`–`$DFF0` | `BREAKPOINT_TABLE_ADDRESS` | ARM↔C64 | Breakpoint table (5 × { addr_lo, addr_hi, opcode }) |

### `instructions.cpp` / `instructions.h`
6502 instruction table. Defines:
- `instructions[]` — array of 57 `Instruction` structs, each with a mnemonic name and up to 10 `{ opcode_byte, AddressingMode }` pairs.
- `g_opcodeToAddressingMode[255]` — fast lookup from opcode byte to addressing mode.
- `g_opcodeToInstructionPointer[255]` — fast lookup from opcode byte to its `Instruction`.
- `AddressingMode` enum (Implied, Immediate, ZeroPage, ZeroPageX, ZeroPageY, Absolute, AbsoluteX, AbsoluteY, IndirectX, IndirectY, Relative, AbsoluteIndirect, Accumulator).
- Used by the disassembler (in `mon.cpp`) and by `determine_next_pc()`.

### `asm.cpp` / `asm.h`
One-line 6502 assembler.

- `assemble(address, input, &ppBytes, &pcBytes)`: Parse a single 6502 instruction string (mnemonic + operand), resolve it against the instruction table, and return the encoded byte(s) in a heap-allocated buffer. Returns 0 on success.
- Used by the `a` command to assemble instructions directly into C64 memory.

---

## Command reference

| Command | Description |
|---------|-------------|
| `a <addr> [insn]` | Assemble instruction(s) at address (interactive line-by-line if no insn given) |
| `b` | Break into monitor via NMI (injects stub, freezes CPU) |
| `bp <addr>` | Set breakpoint at address |
| `bpc [-norestore]` | Clear all breakpoints (restores opcodes unless `-norestore`) |
| `bpd <addr>` | Delete one breakpoint |
| `bps` | Show active breakpoints |
| `cart_o <s> <e> <b…>` | Fill cartridge ROM range with a repeating byte pattern |
| `cart_poke <a> <b…>` | Poke bytes into cartridge ROM |
| `charset <a> <n> [-v]` | Dump character ROM bitmaps (n characters starting at address a) |
| `d [start [end]]` | Disassemble memory (defaults to current PC) |
| `dec <val>` | Convert decimal to hex |
| `dma-freeze` | Assert DMA line / freeze CPU |
| `dma-unfreeze` | De-assert DMA line / resume CPU |
| `dmab` | DMA-sampler break (freeze via sampler observation) |
| `dmar` | DMA-sampler resume |
| `dmaz` | DMA-sampler single step |
| `find "text"` | Convert text to PETSCII and hunt all 64KB, printing every match address and total count |
| `g` | Go: resume from breakpoint (restores opcode, patches PC, signals stub) |
| `h <s> <e> <b…>` | Hunt memory range for a byte pattern |
| `hex <val>` | Convert hex to decimal |
| `info` | Show VIC-II display configuration (bank, screen, bitmap, sprites) |
| `load <filename>` | Load a `.prg` file (first two bytes = load address) |
| `loadbin <f> <addr>` | Load raw binary at a fixed address |
| `m [start [end]]` | Memory dump (hex + ASCII) |
| `nextpc` | Show the next PC value (decode current instruction, compute successor) |
| `nmi` | Trigger NMI freeze (without full break sequence) |
| `nmi-resume` | Signal C64 stub to resume ($DFF1 ← $FF) |
| `o <s> <e> <b…>` | Fill memory range with a repeating byte pattern |
| `peek$1` | Read `$0001` (CPU port) via the on-C64 stub (see note below) |
| `poke <a> <b…>` | Poke one or more bytes into C64 memory |
| `poke$1 <b>` | Write `$0001` (CPU port) via the on-C64 stub (see note below) |
| `r` | Show CPU registers (PC, SR, A, X, Y, SP + flag breakdown) |
| `reset` | Reset the C64 |
| `resume` | Resume from breakpoint without asserting/de-asserting DMA |
| `run` | Type `RUN` + Enter into the C64 keyboard buffer |
| `save <s> <e> <f> [-prg]` | Save memory range to file; `-prg` prepends load address header |
| `setpc <addr>` | Change the saved program counter (patches stack return address) |
| `stack <b>` | Set stack pointer via on-C64 stub |
| `sys <addr>` | Type `SYS <addr>` into the C64 keyboard buffer |
| `x` | Exit monitor |
| `z` | Single-step (NMI path: set breakpoint at next PC, resume, break again) |
| `?` | Show command help |

---

## Breakpoint / single-step protocol

### NMI-based breakpoints

1. ARM sets a `BRK` ($00) opcode at the target address and records the original opcode in the breakpoint table.
2. When the C64 CPU executes `BRK`, the 6502 vectors through `$0316` (BRK vector) to the stub at `$DF00`.
3. The stub saves CPU state to `$DFF0`–`$DFF4` and enters the command loop.
4. `mon` reads registers, lets the user inspect state, then issues `g` or `resume` to restore the original opcode, patch the return PC, and signal the stub to `RTI`.

### DMA-sampler stepping (`dmab` / `dmaz` / `dmar`)

An alternative path that does not require the on-C64 stub. The ARM asserts the `_DMA` line, which freezes the C64 CPU mid-instruction (after the current cycle completes). The Sysop-64 bus sampler captures the bus state so `mon` can observe the CPU's last address/data/control lines. `dmaz` toggles DMA off then back on to advance one cycle at a time.

**Limitation — CPU state is not available in this mode.** Because the stub never runs, the CPU registers (A, X, Y, PC, SR, SP) are never saved to C64 RAM. The `r` command and any other operation that reads from the `$DFF0`–`$DFF4` scratch area will return stale or meaningless values. The sampler output shows bus-level signals (address, data, R/W, PHI2, BA) but these reflect individual bus *cycles*, not a coherent register snapshot. Use the NMI-based break path (`b` / `bp`) when you need to inspect or modify CPU registers.

### Why `peek$1` and `poke$1` exist — the `$0001` limitation

Address `$0001` on the C64 is the **6510 CPU I/O port**, not ordinary RAM. It controls which ROM banks are visible (BASIC, KERNAL, character ROM) and the cassette/serial port lines. Its value is read and written by the CPU's internal I/O registers, not by the address bus.

Because Sysop-64 accesses C64 memory through the **cartridge expansion port**, it sees the memory bus — but the CPU I/O port at `$0001` is internal to the 6510 chip and is **not visible on the expansion bus**. A normal `poke(0x0001, val)` or `peek(0x0001)` will silently read or write whatever RAM happens to be at that physical address instead of the CPU port register.

`peek$1` and `poke$1` work around this by using the on-C64 stub: the ARM writes the desired value to `$DFF2`, sets a command code in `$DFF1`, and the stub (running on the actual 6510) executes `LDA $DFF2 / STA $01` or `LDA $01 / STA $DFF4`. Because the stub runs on the C64's own CPU, its accesses to `$0001` go through the 6510's internal I/O logic and correctly read/write the CPU port.

**Prerequisite**: the stub must already be loaded and running (i.e., the CPU must be frozen at a breakpoint via `b` or `bp`). These commands will hang if the stub is not active.

---

## Adding new commands

1. Add a handler function (or inline the body) in `mon.cpp`.
2. Add the command string to the dispatch table in `main()` (the `if/else if` chain after `get_line()`).
3. Document the command in the help string printed by `?`.
4. If the command needs to access C64 memory, wrap the body in `{ OwnDma odma; … }` — do not call `dma_enable()` / `dma_disable()` directly.

---

## Build

`mon` is built as part of the `tools/` Makefile. From the repository root:

```sh
cd libsysop64
make        # build libsysop64.a first
cd ../tools
make mon    # or just: make
```

The binary is installed to the system path by `make install`.
