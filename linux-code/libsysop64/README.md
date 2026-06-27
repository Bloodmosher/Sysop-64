# libsysop64

C library for controlling a Commodore 64 from the ARM core of a DE-10 Nano (Intel Cyclone V SoC + FPGA). Provides a unified API for hardware bridge access, C64 memory read/write via DMA, cartridge/kernal loading, bus sampling, video palette control, audio playback, keyboard/joystick input, and an IPC client for communicating with the `sysop64` terminal emulator process.

Project home: https://github.com/Bloodmosher/Sysop-64

---

## Architecture overview

```
 ARM Cortex-A9 (Linux userspace)
 ┌────────────────────────────────────────────────────────────┐
 │  Your program / tool                                       │
 │       │                                                    │
 │  libsysop64.a                                              │
 │  ┌──────────────┬──────────────┬──────────────┐            │
 │  │  sysop_bridge│  sysop_server│  sysop_locks │            │
 │  │  (mmap/HW)   │  (TCP :6510) │  (file locks)│            │
 │  ├──────────────┴──────────────┴──────────────┤            │
 │  │  sysop_dma   sysop_memory   sysop_loader   │            │
 │  │  sysop_video sysop_audio    sysop_input    │            │
 │  │  sysop_sampler  sysop_text  sysop_state    │            │
 │  └────────────────────────────────────────────┘            │
 └────────────────────────────────────────────────────────────┘
          │ /dev/mem mmap                │ TCP 127.0.0.1:6510
          ▼                              ▼
 FPGA bridge @ 0xC0000000          sysop64 process
 (register windows, DMA FIFO,      (framebuffer terminal,
  VIC sync, audio engine,           command dispatcher)
  bus sampler, GPIO)
          │
          ▼
 Commodore 64 expansion bus
```

There are **two ways** to talk to the hardware, and which you use depends on context:

1. **Direct bridge** (`sysop_open_bridge` / `sysop_init`): `mmap` the FPGA's Lightweight HPS-to-FPGA bridge at physical address `0xC0000000` into the process's virtual address space via `/dev/mem`. All register reads and writes become ordinary C pointer dereferences. This is the fast path used by `sysop64` itself and any tool that runs with direct hardware access.

2. **IPC client** (`sysop_server_connect` / `sysop_server_*` prefixed functions): Connect to the `sysop64` process's TCP server on `127.0.0.1:6510`. The IPC client is **not** a full remote API — it exists primarily to broker DMA access across processes. Call `sysop_server_dma_lock()` to acquire exclusive DMA ownership from `sysop64`, then use the direct bridge APIs (`sysop_poke`, `sysop_peek`, etc.) as normal. When done, call `sysop_server_dma_unlock()`. A few additional helpers (message display, console close) are also available over this channel.

The direct bridge path is always used for the actual hardware reads and writes; the IPC channel only coordinates access and provides a small set of control commands.

---

## Hardware memory map

All offsets are relative to the bridge base `SYSOP64_BRIDGE = 0xC0000000`.

| Offset | Symbol | Description |
|--------|--------|-------------|
| `0x40000` | `SYSOP64_DMA_WRITE_DATA` | DMA write FIFO data |
| `0x40001` | `SYSOP64_DMA_READ_DATA` | DMA peek result data |
| `0x40002` | `SYSOP64_INTERNAL_READ_DATA` | Internal/IO read result |
| `0x40400` | `SYSOP64_DMA_WRITE_ADDRESS` | DMA write address register |
| `0x40402` | `SYSOP64_DMA_READ_ADDRESS` | DMA peek address register |
| `0x40404` | `SYSOP64_CMD_ADDRESS` | CMD1 register (reset, freeze, cartridge, etc.) |
| `0x40406` | `SYSOP64_INTERNAL_READ_ADDRESS` | Internal/IO read address |
| `0x40800` | `SYSOP64_CMD3_PARAM`    | CMD3 data parameter (write: pass data argument alongside a CMD3 command) |
| `0x40808` | `SYSOP64_FPGA_STATUS`   | Main FPGA status register: DMA queue empty/busy/full/length, VIC-II scanline, cycle counter, sampler busy (read) |
| `0x40810` | `SYSOP64_HDMI_CMD_DATA` | HDMI/video command queue write port (write: palette entries, border colour, extended borders, etc.) |
| `0x40818` | `SYSOP64_PHI2_COUNTER` | PHI2 clock cycle counter |
| `0x40820` | `SYSOP64_C64_SIGNALS` | Raw C64 bus signals snapshot |
| `0x40828` | `SYSOP64_MAP_DEBUG_DATA1` | Debug/vblank register — bit 63 = HDMI vblank active |
| `0x40830` | `SYSOP64_CMD3_RESULT` | Result register for CMD3 commands — bit 63 = busy |
| `0x40838` | `SYSOP64_HDMI_INFO_RESULT` | HDMI timing info result |
| `0x40840` | `SYSOP64_SID_VOICES_DATA` | Live SID voice data |
| `0x40848` | `SYSOP64_GPIO_DATA` | GPIO / button state |
| `0x40850` | `SYSOP64_DMA_READ_KEY_DATA` | C64 keyboard matrix scan result |
| `0x40858` | `SYSOP64_DMA_TAG_DATA` | DMA tag echo |
| `0x40860` | `SYSOP64_DMA_JOYSTICK_DATA` | Joystick port read result |
| `0x40868` | `SYSOP64_AUDIO_STATUS` | Audio playback status |
| `0x40870` | `SYSOP64_AUDIO_COMMAND` | Audio playback command |
| `0x40C00` | `SYSOP64_CMD2_ADDRESS` | CMD2 register (poke/peek/key scan/joystick/DMA tag) |
| `0x40C04` | `SYSOP64_SET_PALETTE_ADDRESS` | HDMI palette write register |
| `0x40C08` | `SYSOP64_CMD3_ADDRESS` | CMD3 register (VIC info, PLL, screenshots, strobes, MIDI, …) |

The **bus sampler** buffer is a separate memory region at physical address `0x21000000` (DDR SDRAM), size 100 MB (`SYSOP64_SAMPLER_BUFFER_SIZE`), mapped on first use.

---

## Source modules

### `sysop_state.c`
Defines every global hardware register pointer variable (e.g. `sysop64_bridge_map`, `sysop64_dma_address_map`, `sysop64_cmd3_map`, `sysop64_debug_data1_map`). These are raw pointers into the mmap'd bridge window. All other modules read/write through these pointers. There are no functions here — it is pure shared state.

### `sysop_bridge.c`
Lifecycle and low-level hardware primitives.

- **`sysop_open_bridge()` / `sysop_init()`**: Opens `/dev/mem`, maps `SYSOP64_BRIDGE_SPAN` bytes at `SYSOP64_BRIDGE` into the process, then populates every pointer in `sysop_state`. Must be called before any other direct-bridge function. Returns 0 on success, negative on error.
- **`sysop_close_bridge()` / `sysop_uninit()`**: Unmaps the bridge window and closes the fd.
- **`sysop_wait_hdmi_vblank()`**: Spin-waits until bit 63 of `sysop64_debug_data1_map` goes high (HDMI vertical blank). Used to synchronize framebuffer flips to the display refresh rate (~60 Hz / ~16 ms period).
- **`sysop_wait_vic()` / `sysop_wait_vic2()` / `sysop_wait_hdmi()`**: Spin-wait until the VIC-II reaches a specific raster line and cycle. Used for cycle-exact C64 timing.
- **`sysop_phi2_counter()`**: Returns the raw PHI2 clock cycle counter from the FPGA.
- **`sysop_read_c64_signals()`**: Returns a 64-bit snapshot of all C64 bus signals (address, data, R/W, PHI2, BA, IRQ, DMA, etc.) at the current moment.
- **`sysop_debug1()` / `sysop_debug2()`**: Read raw values from the debug and status registers for diagnostics.

### `sysop_locks.c`
POSIX advisory file locks shared across processes on the same system.

- **Library lock** (`/tmp/my_staticlib.lock`): Taken by `get_library_lock()` / `release_library_lock()`. Must be held while issuing any CMD3 command that reads back a result (CMD3 writes a command then polls the result register for bit 63 to clear — this is not atomic, so concurrent accesses would race).
- **Framebuffer lock** (`/tmp/sysop_framebuffer.lock`): Taken by `sysop_framebuffer_lock()` / `sysop_framebuffer_unlock()`. Advisory — both the `sysop64` terminal process and external tools use this to coordinate framebuffer access.

### `sysop_dma.c`
C64 DMA write queue management. The FPGA DMA engine writes bytes into C64 memory by asserting the `_DMA` line and driving the bus.

- **`sysop_dma_enable()` / `sysop_dma_disable()`**: Enable/disable the DMA engine.
- **`sysop_dma_freeze()` / `sysop_dma_unfreeze()`**: Freeze/unfreeze the C64 CPU (via NMI or DMA line) while DMA operations run.
- **`sysop_dma_wait_not_busy()`**: Spin until the DMA engine is not actively transferring (bit 52 of `sysop64_fpga_status_map` clears).
- **`sysop_dma_wait_empty()`**: Spin until the write queue is fully drained (bit 63 of `sysop64_fpga_status_map` sets).
- **`sysop_dma_write_queue_length()`**: Returns the current number of pending entries in the DMA write FIFO (bits 35–47 of `sysop64_fpga_status_map`).
- **`sysop_dma_write_tag()` / `sysop_dma_tag_data()`**: Write a tag value into the DMA stream; the FPGA echoes it back so callers can confirm a known point in the queue has been processed.

### `sysop_memory.c`
C64 memory read/write operations.

- **`sysop_poke(addr, val)`**: Queue a DMA write of `val` to C64 address `addr` (CMD2 opcode 1). Waits for the FIFO to have space first.
- **`sysop_peek(addr)`**: Read a byte from C64 memory via the DMA peek path (uses `SYSOP64_DMA_READ_ADDRESS` / `SYSOP64_DMA_READ_DATA`).
- **`sysop_poke_no_wait()`**: Same as `sysop_poke` but skips the FIFO-not-full wait — use only when you know the queue has space.
- **`sysop_cartridge_poke(addr, val)`**: Poke into cartridge ROM space (CMD2 opcode 64 — routes through the DMA manager's cartridge ROM write path).
- **`sysop_kernal_poke(addr, val)`**: Poke into kernal ROM space (CMD2 opcode 65).
- **`sysop_internal_peek()` / `sysop_io_peek()` / `sysop_io_poke()`**: Peek/poke the I/O and internal address spaces via CMD3.

### `sysop_loader.c`
File loading into C64 memory.

- **`sysop_load(filename)`**: Load a `.prg` file (first two bytes = little-endian load address). Uses `sysop_poke()` + verify loop.
- **`sysop_loadbin(filename, address)`**: Load a raw binary file at a fixed address. Verifies every byte after write.
- **`sysop_load_buffer()` / `sysop_load_buffer_at()`**: Load from an in-memory byte array.
- **`sysop_cartridge_load(filename, verifyOnly)`**: Load a `.crt` (CRT format) or raw binary cartridge ROM. Parses the CRT header and CHIP packets. `verifyOnly` re-reads and compares without writing.
- **`sysop_cartridge_enable(rom_size)` / `sysop_cartridge_enable_ultimax()` / `sysop_cartridge_disable()`**: Enable/disable cartridge mode via CMD1 commands.
- **`sysop_kernal_load(filename, verifyOnly)`**: Load a replacement kernal ROM image.

### `sysop_video.c`
HDMI output, palette, and miscellaneous video/hardware commands.

- **`sysop_set_palette_entry(index, r, g, b)`**: Write a C64 color palette entry (0–15) to the FPGA's HDMI output pipeline. Direct write to `SYSOP64_SET_PALETTE_ADDRESS`.
- **`sysop_get_palette_entry(index, &r, &g, &b)`**: Read a palette entry back via CMD3 `SYSOP64_CMD3_ID_GET_COLOR`. Takes the library lock.
- **`sysop_wait_set_palette_entry()` / `sysop_queue_set_palette_entry()`**: Timed or queued palette writes synchronized to a VIC raster position.
- **`sysop_hdmi_set_extended_borders()`** / **`sysop_queue/wait_set_extended_border_color*()`**: Control the HDMI extended border color (area outside the C64's visible screen).
- **`sysop_framebuffer_show()` / `sysop_framebuffer_hide()` / `sysop_framebuffer_flip()`**: Show/hide/flip the ARM framebuffer overlay on the HDMI output. `sysop_framebuffer_flip()` waits for vblank then issues CMD1 `SYSOP64_CMD_ID_FLIP_FRAMEBUFFER`.
- **`sysop_get_vic_info()` / `sysop_is_pal()`**: Query the VIC-II chip type and PAL/NTSC status. Returns one of `VIC_CHIP_6567R56A`, `VIC_CHIP_6567R8`, `VIC_CHIP_6569`, `VIC_CHIP_6572RO_DREAN`.
- **`sysop_screenshot_request()` / `sysop_screenshot_status()` / `sysop_screenshot_read()`**: Trigger and read an FPGA-captured screenshot of the C64 video output.
- **`sysop_video_reset()`**: Reset the FPGA video pipeline.
- **`sysop_set_dma_timing_ntsc()` / `sysop_set_dma_timing_pal()`**: Adjust DMA access timing for NTSC or PAL C64s.

### `sysop_audio.c`
FPGA audio playback engine. PCM audio data is streamed from DDR SDRAM to the HDMI audio output.

Commands are 64-bit writes to `SYSOP64_AUDIO_COMMAND`: bits 63–56 = command opcode, bits 55–0 = payload.

- **`sysop_audio_set_base_addr(addr)`**: Set the DDR SDRAM start address of the PCM buffer.
- **`sysop_audio_set_length_frames(n)`**: Set the playback length in sample frames.
- **`sysop_audio_set_loop_enable(enable)`**: Enable/disable looping.
- **`sysop_audio_start()` / `sysop_audio_stop()`**: Start/stop playback.
- **`sysop_audio_play_pcm(addr, length, loop)`**: Convenience wrapper that sets address, length, loop, and starts in one call.
- **`sysop_audio_wait_until_done()`**: Block until the playing flag clears.
- **`sysop_audio_is_playing()` / `sysop_audio_has_underrun()` / `sysop_audio_get_underrun_count()`**: Status queries from `SYSOP64_AUDIO_STATUS`.

### `sysop_input.c`
C64 keyboard and joystick input, plus GPIO buttons.

- **`sysop_read_key_data()`**: Issues CMD2 opcode 66 to trigger a C64 keyboard matrix scan in the FPGA's DMA manager, then reads the result from `SYSOP64_DMA_READ_KEY_DATA`. Returns a 64-bit bitmask of pressed keys.
- **`sysop_scan_keys()` / `sysop_is_key_down()` / `sysop_is_shift_key_down()`**: Higher-level key state API. `sysop_scan_keys()` updates an internal snapshot; `sysop_is_key_down(rawKeyIndex)` tests a bit in that snapshot.
- **`sysop_read_joystick(joystick_number)`**: Issues CMD2 opcode 68 to read joystick port 1 or 2. Returns a byte with the standard active-low C64 joystick bits.
- **`sysop_is_button_pressed(id)`**: Tests the DE-10 Nano's GPIO push buttons via `SYSOP64_GPIO_DATA`.
- **`sysop_any_key_down()`**: Tests whether any key is currently pressed by scanning the C64 keyboard matrix via `poke`/`peek` on the CIA registers.

### `sysop_sampler.c`
C64 bus logic analyzer. The FPGA continuously captures bus signals; a trigger causes it to latch a snapshot of up to 12.5 million cycles into DDR SDRAM at `0x21000000`.

- **`sysop_sampler_start()`**: Send CMD1 `SYSOP64_CMD_ID_TRIGGER_SAMPLER` to arm and trigger a capture.
- **`sysop_sampler_wait_not_busy()`**: Spin until the sampler has finished writing (bit 55 of `sysop64_fpga_status_map` clears).
- **`sysop_sampler_get_sample(index, &sample)`**: Map the sample buffer on first call, then decode sample `index` from the 64-bit raw word into a `sysop_c64_bus_sample` struct. Fields include `addr`, `data`, `r__w`, `phi2`, `ba`, `_irq`, `_dma`, `freeze_signal`, `vic_line`, `cycle`, `_roml`, `_romh`, `_io1`, `_io2`, `_charen`, `_hiram`, `_loram`, `_exrom`, `_game`.

### `sysop_midi.c`
Namesoft MIDI interface support.

- **`sysop_namesoft_midi_ready()`**: Poll via CMD3 opcode 25 whether the Namesoft MIDI interface is ready to accept a new byte.
- **`sysop_namesoft_midi_write(data)`**: Write a MIDI byte via CMD3 opcode 26 and assert NMI so the C64 handles it.
- **`sysop_set_nmi_vector(addr)`**: Set the C64 NMI vector via CMD3.

### `sysop_text.c`
Character set conversion between ASCII and PETSCII (the C64's character encoding). Provides bidirectional lookup tables initialized on first use.

- **`sysop_map_ascii_to_petscii(c)`** / **`sysop_map_petscii_to_ascii(c)`**: Single-character conversion.
- **`sysop_screen_clear(baseAddr)`**: Clear a C64 screen RAM region using `sysop_poke()`.

### `sysop_server.c`
TCP IPC client that talks to the `sysop64` process's command server (`127.0.0.1:6510`). This is **not** a full remote API — its primary purpose is DMA access brokering between processes. Once a lock is acquired, the caller uses the direct bridge APIs for all hardware access. A small number of additional control commands are also available.

- **`sysop_server_connect()` / `sysop_server_disconnect()`**: Open/close the TCP connection. Sets `TCP_NODELAY` for low-latency command dispatch.
- **`sysop_server_dma_lock()` / `sysop_server_dma_lock2()` / `sysop_server_dma_unlock()`**: Acquire/release exclusive DMA access from `sysop64`. After locking, use the direct bridge functions (`sysop_poke`, `sysop_peek`, `sysop_dma_*`, etc.) directly — no IPC overhead for individual memory operations.
- **`sysop_server_display_message()` / `sysop_server_hide_messages()` / `sysop_server_queue_hide_messages()`**: Display/hide an on-screen overlay message via the sysop64 framebuffer.
- **`sysop_server_console_close()`**: Tell sysop64 to close the PTY console session.

---

## Public headers

| File | Purpose |
|------|---------|
| `include/sysop64.h` | Top-level include — pulls in `sysop_defines.h` and `sysop_library.h` |
| `include/sysop_defines.h` | Hardware constants: `SYSOP64_BRIDGE` address, all register offsets, CMD1/CMD2/CMD3 opcodes, VIC chip IDs |
| `include/sysop_library.h` | Full public API declarations and the `sysop_c64_bus_sample` struct |
| `include/c64keys.h` | C64 key code constants for use with `sysop_is_key_down()` |
| `include/keyboard.h` | Additional keyboard mapping helpers |

---

## Build

Built on the DE-10 Nano (ARM Linux). Requires `gcc` and `ar`.

```sh
# Build libsysop64.a → ../build/lib/libsysop64.a
make

# Build the companion tools in ../tools/ as well
make tools

# Install tools to the system
make install

# Clean object files and the archive
make clean
```

**Important**: always rebuild after changing any source file in `src/` or `include/`. The `sysop64` terminal emulator links against `../build/lib/libsysop64.a` at link time; a stale archive will silently use the old code even if the sources have changed. This has caused hard-to-diagnose bugs (e.g. `sysop64_debug_data1_map` being NULL because the archive predated the assignment in `sysop_open_bridge()`).

---

## Common patterns

### IPC client (external process — DMA brokering)

```c
#include "sysop64.h"

int main() {
    // Open both the direct bridge AND the IPC channel
    if (sysop_init() != 0) return 1;
    if (sysop_server_connect() != 0) return 1;

    // Acquire DMA access from sysop64, then use direct bridge APIs
    sysop_server_dma_lock();
    sysop_poke(0x0400, 0x01);         // direct bridge write
    uint8_t val = sysop_peek(0x0400); // direct bridge read
    sysop_server_dma_unlock();

    sysop_server_disconnect();
    sysop_uninit();
    return 0;
}
```

### Direct hardware access (runs as part of a bridge-capable process)

```c
#include "sysop64.h"

int main() {
    if (sysop_init() != 0) { /* sysop_open_bridge failed */ return 1; }

    // Write a byte to C64 RAM at $0400
    sysop_dma_enable();
    sysop_poke(0x0400, 0x01);
    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();

    // Read it back
    uint8_t val = sysop_peek(0x0400);

    sysop_uninit();
    return 0;
}
```

### Sampler / bus capture

```c
sysop_dma_enable();
sysop_sampler_start();
sysop_sampler_wait_not_busy();

for (uint32_t i = 0; i < 1000; i++) {
    struct sysop_c64_bus_sample s;
    sysop_sampler_get_sample(i, &s);
    printf("line=%d cycle=%d addr=%04X data=%02X rw=%d\n",
           s.vic_line, s.cycle, s.addr, s.data, s.r__w);
}
```

---

## Threading and locking notes

- The library is **not thread-safe by default**. The library lock (`/tmp/my_staticlib.lock`) is a cross-*process* advisory lock, not a mutex. Within a single multi-threaded process, callers must add their own synchronization if multiple threads call library functions concurrently.
- CMD3 commands (e.g. `sysop_get_palette_entry`, `sysop_get_vic_info`) take the library lock for the duration of the command-and-result cycle. Do not call other CMD3 functions from a signal handler while the lock may be held.
- `sysop_wait_hdmi_vblank()` is a busy-spin. Do not hold the library lock while calling it.
- The framebuffer lock (`/tmp/sysop_framebuffer.lock`) is advisory between processes only. `sysop64` holds it while rendering; external processes should acquire it before writing to framebuffer-related hardware.
