<!--
Sysop-64
https://github.com/Bloodmosher/Sysop-64

SPDX-License-Identifier: MIT
Copyright (c) 2026 Sysop-64 Project
-->
# Port 01 Monitor Debugging Notes

This note explains the purpose of the Sysop-64 `$0001` / CHL monitor, how the FPGA and HPS tools fit together, and the usual workflow for debugging mismatches. It is meant for future developers and future agentic coding sessions that need to pick this work back up without rediscovering the whole trail.

## Key Idea

On a C64, the 6510 CPU has an internal I/O port at `$0000/$0001`:

- `$0000` is the data-direction register for the CPU port.
- `$0001` is the output latch.
- `$0001` bits 2, 1, and 0 control `CHAREN`, `HIRAM`, and `LORAM`.

Those three low bits decide which parts of the C64 memory map are visible: RAM, BASIC ROM, KERNAL ROM, character ROM, and I/O space. Sysop-64 originally used physical wires for `_charen`, `_hiram`, and `_loram`. The long-term goal of `c64_bus_monitor.v` is to infer those values from the cartridge-port bus instead, so the extra wires can become optional.

The hard part is that writes to `$0001` are special. The actual low-bit CPU-port state is internal to the 6510 and is not reliably visible as ordinary external data on the cartridge port. So the FPGA monitor follows enough of the 6502/6510 instruction stream to know what value A/X/Y should contain when code stores to `$0001`.

Example easy case:

```asm
lda #$37
sta $01
```

The bus does not have to expose the correct `$37` on the write cycle if the monitor already knows A is `$37`.

A more subtle case involves `$0000`:

```asm
lda #$00
sta $00      ; low CPU-port bits become inputs
sta $01      ; latch changes, but CHL does not get driven low
```

For CHL purposes, the effective low bits are:

```text
effective_chl_bit = ddr_bit ? latch_bit : 1
```

So the monitor must track both the `$0000` DDR and the `$0001` latch. The exported `inferred_port_01[2:0]` should be treated as the effective CHL value after applying the DDR, because that is what the rest of the system actually needs.

## FPGA Pieces

### `rtl/c64/c64_bus_monitor.v`

This is the core inference engine. It watches sampled C64 bus cycles and maintains shadow state for:

- Current instruction stream position.
- A, X, and Y when needed for later stores.
- A small amount of flag state, mostly for branch handling.
- `$0000` CPU port DDR.
- `$0001` CPU port latch.
- Effective inferred `$0001` / CHL value.
- Debug breadcrumbs for the last `$0001` write and the instruction that produced it.

Important design principle: do not advance CPU instruction state while `_dma` says the FPGA owns the bus. During DMA, the address/data bus may contain useful transfer traffic, but it is not C64 CPU execution.

### `c64_tick_sampler.v`

This is the normal trace sampler used by `tickstream`. It records selected C64 bus samples into SDRAM and includes status bits such as the inferred-vs-physical CHL match result. Use this for normal mismatch capture and for broad replay/debug traces.

### `c64_bus_monitor_debug_sampler.v`

This is the heavier debug sampler. It emits additional internal monitor state so `scripts/decode_busmon_debug.py` can show what the monitor thought was happening cycle by cycle. Use this when the normal trace tells you where a mismatch occurred but not why.

### `sysop64_top.v`

This wires the monitor into the wider FPGA design. The system can compare physical CHL wires against `inferred_port_01[2:0]`, and can optionally route the inferred value into the rest of the system instead of using the physical wires.

## Tooling Map

### `port01/port01.c`

Small HPS-side status tool. It reads the FPGA monitor status and prints the inferred port value, actual physical CHL value, match status, sync status, and debug breadcrumb fields.

Useful when you want a quick snapshot after booting, resetting the monitor, running a test program, or enabling/disabling monitor-driven CHL.

Typical use:

```sh
../build/tools/port01
../build/tools/port01 --reset-monitor
```

### `tickstream.c`

HPS-side streaming server for C64 bus traces. It starts the FPGA sampler, streams samples over TCP, and can stop when the FPGA reports a `$0001` / CHL mismatch.

Typical mismatch capture:

```sh
../build/tools/tickstream 9100 --exit-on-01-mismatch --busmon-debug --tick 44
```

Optional reset capture:

```sh
../build/tools/tickstream 9100 --exit-on-01-mismatch --reset-on-client --busmon-debug --tick 44
```

Use `--reset-on-client` when you specifically want the capture to include reset and monitor sync behavior. You do not always need it. If the monitor is already synced and you are debugging a later program behavior, a normal run is often better.

### `scripts/tickstream_client.py`

PC-side client that connects to `tickstream` and writes the incoming stream to a capture file.

Example:

```sh
python scripts/tickstream_client.py capture-01.bin 10.0.0.74 --port 9100
```

### `scripts/analyze_port01_chl.py`

High-level trace analyzer for normal tickstream captures. Use this first. It reports reset anchoring, CHL match/mismatch transitions, DMA/IRQ state, nearby bus cycles, and the instruction context around the first mismatch.

Example:

```sh
python scripts/analyze_port01_chl.py capture-01.bin
```

### `scripts/decode_busmon_debug.py`

Detailed decoder for bus-monitor debug captures. Use this when `scripts/analyze_port01_chl.py` points to a suspicious area but you need to see the monitor state machine, shadow registers, current opcode, source codes, or event flags.

Examples:

```sh
python scripts/decode_busmon_debug.py capture-01.bin --mismatches-only --count 40
python scripts/decode_busmon_debug.py capture-01.bin --start 5510600 --count 120
python scripts/decode_busmon_debug.py capture-01.bin --events-only --count 100
```

### `scripts/sim_c64_bus_monitor.py`

Python-side model of the bus monitor. This is useful for experimenting with inference behavior against an existing capture without rebuilding the FPGA. It is not a replacement for the Verilog, but it can help prove a theory before editing RTL.

### `port01/port01_monitor_test.asm`

ACME-compatible C64 test program containing focused regression cases for tricky `$0001` monitor behavior. Add a case here whenever a real program exposes a new instruction pattern that broke inference.

Good test categories include:

- `LDA/LDX/LDY` followed by stores to `$01`.
- Immediate `ORA/AND/EOR` sequences.
- `BIT` and branch behavior.
- `INC/DEC/ASL/LSR $01`.
- Stack, RTS, RTI, and IRQ paths.
- Indexed zero-page stores that hit `$0000` or `$0001`.
- `$0000` DDR changes that affect effective CHL.

Build from the tools folder:

```sh
make ../build/tools/port01_monitor_test.prg
```

Then load/run it using the normal Sysop-64 workflow. While it loops, run `tickstream --exit-on-01-mismatch`; if it never exits, the current regression set is passing.

## Recommended Debug Workflow

1. Reproduce with physical CHL wires still connected.

   The physical wires are the ground truth while developing the monitor. Even if the system is configured to use inferred CHL, keep the comparison path available.

2. Capture the first mismatch.

   On the Sysop-64 side:

   ```sh
   ../build/tools/tickstream 9100 --exit-on-01-mismatch --busmon-debug --tick 44
   ```

   On the PC side:

   ```sh
   python scripts/tickstream_client.py capture-01.bin 10.0.0.74 --port 9100
   ```

3. Run the high-level analyzer.

   ```sh
   python scripts/analyze_port01_chl.py capture-01.bin
   ```

   Look for:

   - The reset anchor / sync point.
   - The first `match=1` to `match=0` transition.
   - The nearby reads/writes to `$0000` and `$0001`.
   - Whether `_dma` was asserted or released.
   - Whether `_irq` was involved.

4. Decode internal monitor state if needed.

   ```sh
   python scripts/decode_busmon_debug.py capture-01.bin --start <sample> --count 150
   ```

   Check:

   - `op`, `cur`, and PC fields.
   - Shadow A/X/Y values.
   - `p01` effective inferred value.
   - Source/event flags such as `readcap`, `writeinf`, and `port01`.
   - Whether the monitor advanced during DMA-owned bus cycles.

5. Explain the CPU instruction sequence in plain terms.

   Before editing RTL, write down what the C64 code is doing. For example:

   ```asm
   lda #$00
   ldx #$00
   sta $00,x   ; writes DDR
   inx
   sta $00,x   ; writes $0001 latch
   ```

   Then compare that with what the monitor thought happened.

6. Add a focused regression to `port01/port01_monitor_test.asm`.

   Do this before or alongside the RTL fix. The best cases are small and intentional, with a marker value so a trace is easy to locate.

7. Make the smallest RTL fix that matches the trace.

   Avoid weakening sync, trusting `$0001` bus data, or adding broad guesses. Prefer explicit handling of the instruction/cycle case you can prove.

8. Retest both the small regression and a real program trace.

   The small test prevents immediate regression. The real trace tells you whether the monitor still behaves in the messy environment that found the bug.

## Common Gotchas

### `$0001` bus data is not enough

Do not infer low CHL bits by blindly copying the cartridge-port data byte on a `$0001` write. The monitor exists because that is unreliable.

### `$0000` matters

The DDR can make low `$0001` bits inputs. In that case, the latch can contain one value while the effective CHL lines behave high. Track DDR and latch separately.

### DMA cycles are not CPU execution

If `_dma` indicates the FPGA owns the bus, do not advance the instruction decoder. Some false-reset or false-opcode bugs come from treating DMA bus activity as CPU cycles.

### IRQ pin state is not the same as IRQ entry

The monitor should recognize interrupt entry from bus sequence behavior, not only from `_irq` being low.

### Branch handling is fragile

Most branches can be inferred from bus addresses. Ambiguous cases need careful flag tracking. Do not casually make branch logic more clever without traces for taken, not-taken, page-crossing, and branch-after-branch cases.

### Debug breadcrumbs are part of the design

When adding a new inference path, keep or improve the breadcrumb fields. Future failures are much easier when `port01` can say which opcode/source/PC produced the bad value.

## What To Tell A Future Agent

If you are a future coding agent picking this up:

- Start by reading `rtl/c64/c64_bus_monitor.v` top comment.
- Then inspect `port01/port01.c`, `scripts/analyze_port01_chl.py`, `scripts/decode_busmon_debug.py`, and `port01/port01_monitor_test.asm`.
- Do not remove physical CHL comparison while debugging.
- Do not trust `$0001` low bits from raw bus data.
- Do not advance CPU state while DMA owns the bus.
- If a new real trace fails, first add a tiny regression case to `port01/port01_monitor_test.asm`.
- Keep fixes trace-driven and narrow.

The monitor is intentionally not a full 6510 emulator. It is a practical bus-following inference engine. The right standard is not elegance in isolation; it is staying aligned with real bus cycles well enough to infer effective CHL reliably.
