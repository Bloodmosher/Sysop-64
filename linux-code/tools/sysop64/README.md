# sysop64

The `sysop64` process is the core runtime application for the Sysop-64 hardware
interface. It runs persistently on the DE-10 Nano (ARM/FPGA host) and provides:

- An interactive C64 console: a full terminal emulator displayed on the FPGA
  framebuffer, with a real login shell running behind it.
- An on-screen message display panel with a typing-animation effect and a logo.
- A TCP command server that lets other tools (via `libsysop64`) request DMA
  access, send display messages, and control the console.

---

## High-Level Architecture

```
 Physical keyboard (evdev)
          │
          ▼
   [keyboard monitor]──────────────────────┐
   (inotify hot-plug)                      │ input_event structs
                                           ▼
                                     [pty_loop]  ◄──── console button (FPGA GPIO)
                                      (select)
                                      │     │
                           PTY output │     │ keystrokes
                                      ▼     ▼
                             [terminal  ]  [PTY master write]
                             [ parser  ]         │
                                  │              ▼
                             line buffer    /usr/bin/login
                              (CircularBuffer)   (child process,
                                  │               slave PTY)
                                  ▼
                           [framebuffer renderer]
                           Cairo + Pango → FPGA mmap'd framebuffer
                           (double-buffered, ~60 fps)

 libsysop64 clients (other tools on the same machine)
          │ TCP port 6510 (loopback only)
          ▼
   [TCP command server]
   (handleAccept / handleClient)
          │
          ├─ DMA lock/unlock ──► recursive mutex + dma_enable/disable
          ├─ Show/hide message ─► message queue ──► message display panel
          └─ Console close ─────► console_close_requested flag
```

---

## Threads

| Thread | Entry point | Purpose |
|--------|-------------|---------|
| Main | `main()` | Initialisation, then loops calling `pty_loop()` |
| PTY loop | `pty_loop()` (called from main) | select() event loop, keyboard I/O, terminal emulation |
| Command server accept | `handleAccept` (pthread) | Accepts TCP connections on port 6510 |
| Per-client handler | `handleClient` (pthread, detached) | One thread per connected libsysop64 client |

The main thread is single-threaded for all rendering and DMA access. The client
handler threads communicate back to the main thread only through a small set of
`volatile int` flag variables and a mutex-protected message queue — no direct
rendering or DMA calls happen from client threads.

---

## Display Modes

sysop64 has two mutually exclusive display modes, each using the same
double-buffered FPGA framebuffer hardware.

### Console mode

Activated by pressing the physical console button (button 2 on the FPGA GPIO) or
by a `SYSOP_SERVER_CMD_DMA_LOCK` command with `close_console = 0`.

- Shows a full-screen terminal overlay (green text on black, monospace font).
- The C64 keyboard matrix is scanned every loop iteration via DMA poke/peek; key
  events are emitted as Linux `input_event` structs and fed back into the PTY.
- DMA is owned for the duration of the console session.
- Deactivates automatically when `console_close_requested` is set.

### Message display mode

Activated when a `SYSOP_SERVER_CMD_SHOW_MESSAGE` command is received and the console is
not active.

- Shows the Sysop-64 logo image and an animated typing-effect message.
- Messages are queued; each displays with a configurable timeout before the next
  is dequeued.
- Uses "ROG Fonts" for the message text and Cairo for the eye animation.
- Does not require DMA ownership.

---

## Double Buffering

Two ARGB framebuffer pages are mmap'd from `/dev/mem`:

| Symbol | Physical address | Role |
|--------|-----------------|------|
| `pFrameBuffer1` | `MEM_ADDRESS1` (0x20000000) | Page 1 |
| `pFrameBuffer2` | `MEM_ADDRESS2` (0x207e9000) | Page 2 |

Cairo image surfaces and Pango layouts are created once for each page (`g_cr1` /
`g_cr2`, `g_layout1` / `g_layout2`). The active draw pointers (`pFrameBuffer`,
`g_cr`, `g_layout`) always point at the **back** (invisible) buffer.

After drawing, `framebuffer_flip()` tells the FPGA to display the page we just
drew. Then `wait_hdmi_vblank()` waits for the top-of-frame signal so the flip
completes cleanly. Finally the back/front pointers are swapped. The new back
buffer is immediately redrawn to keep both pages in sync (so a re-flip never
shows a stale page).

---

## Terminal Emulator

`process_buffer()` in `sysop64_terminal_parser.cpp` is a state-machine VT100/ANSI
parser. It handles:

- Printable characters — written to the current line in `CircularBuffer`.
- `\r`, `\n`, `\b`, `\t` — carriage return, newline, backspace, tab.
- **ESC sequences** — cursor save/restore (`ESC 7` / `ESC 8`), keypad mode.
- **CSI sequences** (`ESC [`) — cursor movement (CUU/CUD/CUF/CUB/CUP), erase
  (ED/EL), insert/delete lines and characters, scroll regions (DECSTBM).
- **OSC sequences** (`ESC ]`) — terminal property queries (e.g. foreground colour).
- **DCS sequences** (`ESC P`) — consumed and discarded.

The line store is a `CircularBuffer`: a `std::vector<std::string>` of exactly
`MAX_VISIBLE_LINES` (33) rows, each padded to `term_cols` (106) characters.
`cursor_row` and `cursor_col` are 1-based. `scroll_top` tracks the top of the
active scroll region.

Dirty tracking: each line has a `redraw_needed[]` flag; `redraw_all` forces a
full repaint. The renderer skips lines that are neither dirty nor part of a full
redraw.

---

## DMA Mutex and Locking

Access to the C64 hardware bus (DMA) is coordinated through a POSIX recursive
mutex (`lock` in `sysop64_server.cpp`).

- `console_acquire_lock()` — used by the console mode on the main thread.
- `acquire_lock(clientSocket)` / `release_lock(clientSocket)` — used by client
  handler threads in response to `SYSOP_SERVER_CMD_DMA_LOCK` / `SYSOP_SERVER_CMD_DMA_UNLOCK`.

DMA enable/disable are reference-counted (`dma_refCount`, `per_thread_dma_refCount`)
so nested lock/unlock pairs from multiple clients don't prematurely release the bus.

When a client requests `SYSOP64_DMA_LOCK` while the console is active, the server sets
`console_yield_lock = 1` and the main loop yields in the next iteration. When the
client later calls `SYSOP64_DMA_UNLOCK`, `console_reacquire_lock = 1` is set so the main
loop picks the bus back up.

---

## Keyboard Handling

Two separate keyboard translation paths exist:

| Path | Used when | Entry point |
|------|-----------|-------------|
| `map_key()` | Normal terminal typing (PTY input) | `sysop64_input_mapping.cpp` |
| `map_key_with_c64_mapping()` | C64 console mode legacy path | `sysop64_input_mapping.cpp` |
| `scan_keys_pipe()` | C64 console mode (physical C64 keyboard matrix) | `sysop64_c64_keyboard.cpp` |

`map_key()` uses pre-built lookup tables (`ctrl_map[]`, `shift_map[]`,
`normal_map[]`) populated by `init_key_mapping_tables()` at startup.

`scan_keys_pipe()` drives CIA1 ports via DMA (poke/peek to `$DC00`/`$DC01`) to
scan all 8 rows of the hardware C64 keyboard matrix. State changes are emitted as
`input_event` structs on a pipe, which `pty_loop()` reads with `select()`.

Hot-plug is handled by `sysop64_keyboard_monitor.cpp` using inotify watches on
`/dev` and `/dev/input`. The first evdev device reporting KEY_Q + KEY_W is opened
and exclusively grabbed (`EVIOCGRAB`).

---

## TCP Command Server (port 6510)

The server listens on `127.0.0.1:6510` (loopback only). Each client connection
gets a dedicated `handleClient` thread. Commands are single-byte opcodes defined
in `sysop_library.h` (`SYSOP_SERVER_CMD_*`). Commands that require acknowledgement send
back a single `0x01` response byte.

| Opcode | Name | Payload | Ack |
|--------|------|---------|-----|
| `SYSOP_SERVER_CMD_DMA_LOCK` | Lock DMA bus | 1 byte: close_console flag | Yes |
| `SYSOP_SERVER_CMD_DMA_UNLOCK` | Unlock DMA bus | — | Yes |
| `SYSOP_SERVER_CMD_CONSOLE_CLOSE` | Close the console overlay | — | No |
| `SYSOP_SERVER_CMD_SHOW_MESSAGE` | Queue a display message | 4-byte timeout (ms), 1-byte len, N bytes text | No |
| `SYSOP_SERVER_CMD_HIDE_MESSAGE` | Immediately hide the panel | — | No |
| `SYSOP_SERVER_CMD_QUEUE_HIDE_MESSAGE` | Enqueue a hide command | — | No |

Additional commands (`POKE`, `PEEK`, `MEMORY_WRITE`, `LOAD`, etc.) are handled
by `libsysop64` on the client side and sent via the same socket — see
`libsysop64/src/sysop_server.c` for the client implementations.

---

## File Map

| File | Contents |
|------|----------|
| `sysop64_main.cpp` | `main()`: init sequence, starts server thread, pty loop |
| `sysop64_state.cpp` | All global variable definitions; `set_raw_mode`, `error_exit` |
| `sysop64_internal.h` | Shared includes, constants, struct definitions, all `extern` decls |
| `sysop64_pty.cpp` | `pty_loop()`: PTY creation, fork, select() event loop |
| `sysop64_terminal_parser.cpp` | `process_buffer()`: VT100/ANSI escape sequence parser |
| `sysop64_terminal_render.cpp` | `render_lines()`, `draw_lines()`: Cairo/Pango text rendering |
| `sysop64_framebuffer.cpp` | `update_framebuffer()`, `draw_to_context()`, mouse cursor |
| `sysop64_display.cpp` | `toggle_ui_visibility()`, message display panel, console button |
| `sysop64_server.cpp` | TCP accept/client threads, DMA mutex, message queue dispatch |
| `sysop64_c64_keyboard.cpp` | C64 keyboard matrix scan, `map_c64_key()` |
| `sysop64_input_mapping.cpp` | Host key translation tables, `map_key()`, scroll helpers |
| `sysop64_keyboard_monitor.cpp` | inotify hot-plug detection, keyboard open/grab |

---

## Key Constants (sysop64_internal.h)

| Constant | Value | Meaning |
|----------|-------|---------|
| `MAX_VISIBLE_LINES` | 33 | Terminal rows in the line buffer |
| `LINE_WIDTH` | 255 | Max characters per line |
| `MEM_ADDRESS1` | 0x20000000 | FPGA framebuffer page 1 physical base |
| `MEM_ADDRESS2` | 0x207e9000 | FPGA framebuffer page 2 physical base |
| `MEM_SIZE` | 100 MB | Size of each framebuffer mmap region |
| `SYSOP_SERVER_PORT` | 6510 | TCP port for the command server |

---

## Build

`sysop64` is built by the `tools/Makefile` target `sysop64`. It links against
Cairo, PangoCairo, Pango, GLib, GObject, libevdev, libm, libudev, and
`libsysop64` (the local static library under `build/lib/`).

```sh
# From the repo root:
make -C tools sysop64
```

The resulting binary is placed in `build/tools/sysop64`.

---

## Runtime Requirements

- Must be run as root (or with appropriate capabilities) to open `/dev/mem` and
  grab input devices with `EVIOCGRAB`.
- Requires the FPGA framebuffer to be initialised and mapped at the expected
  physical addresses.
- `/usr/local/bin/sysop-msg-1.png` must exist for the message display logo.
- The sysop64 binary is installed to `/usr/local/bin/` by `make install`.
