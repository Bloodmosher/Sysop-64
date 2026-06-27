# sysop_menu

Interactive file-browser and game launcher for the Sysop-64 system.

`sysop_menu` runs continuously on the ARM Linux host. While idle it shows an
animated HDMI overlay (starfield + Sysop logo). Pressing hardware button 1
opens a file browser that lets the user navigate the host filesystem and D64
disk images and launch programs directly onto the C64.

---

## Architecture overview

```
main.cpp          — init, outer/inner event loops, key dispatch, program launch
display.cpp/.h    — double-buffered HDMI framebuffer, Cairo/Pango rendering
file_browser.cpp/.h — directory listing, D64 browsing, scroll/selection state
disk.cpp          — D64 image parser and loader (via libsysop64 DMA)
keyboard.c        — C64 keyboard matrix scanner (reads hardware via libsysop64)
stars.c/.h        — animated 3-D starfield (background of the overlay)
```

---

## Module details

### main.cpp

Entry point and top-level control flow.

**Startup sequence:**
1. Install `SIGINT`/`SIGTERM` handlers.
2. Call `initIncTable()` to generate the starfield velocity wave table.
3. `sysop_init()` / `sysop_connect()` — open the libsysop64 connection.
4. `mmap` the two physical HDMI framebuffers (`/dev/sysop-fb`).
5. Create one Cairo surface + Pango layout per buffer (double-buffering).
6. Load the Sysop logo PNG from `/usr/local/bin/sysop-logo-480x196.png`.

**Outer loop** — waits for hardware button 1 to be pressed and released, then
opens the file browser.

**Inner loop** (file browser active):
- Calls `update()` at ~50 Hz for animation and `update_framebuffer()` every
  iteration to drive the display.
- Reads the C64 keyboard matrix via `system_scan_keys()`.
- Key bindings:

  | Key | Action |
  |-----|--------|
  | Cursor Down / Up | Move selection |
  | F7 / Shift-F7 | Page down / jump to end |
  | F1 / Shift-F1 | Page up / jump to top |
  | Return | Open folder / D64 / launch file |
  | ESC | Exit file browser |
  | Hardware button 1 | Exit file browser |

**Program launch flow (Return on a file):**
1. `sysop_c64_reset()` — reset the C64 via hardware.
2. Wait for the KERNAL to initialise (~2 s).
3. If the file is inside a D64: `d64_load(d64path, filename)` — DMA the file
   from the disk image into C64 RAM.
4. If the file is a raw PRG: `load_prg()` equivalent via libsysop64.
5. `inject_run()` — write "RUN\r" into the C64 keyboard buffer so BASIC
   auto-runs the loaded program.

**`inject_run()`** pokes the C64 keyboard buffer at `$0277–$027A` with the
PETSCII codes for R, U, N, CR and sets the pending-keys count at `$00C6 = 4`.

---

### display.cpp / display.h

Manages the HDMI overlay using a double-buffered Cairo pipeline.

**Buffers:**  
Two `mmap`-ed regions of `/dev/sysop-fb` (`pFrameBuffer1` / `pFrameBuffer2`),
each holding a 1920×1080 ARGB32 frame. One buffer is shown on screen while
the other is being drawn into. `flip_buffers()` calls `framebuffer_flip()`
(libsysop64) to swap which buffer is visible, then rotates the active Cairo
context and Pango layout pointers.

**Per-frame render (`update_framebuffer`):**
1. Clear to black.
2. `drawStars()` — perspective-projected starfield.
3. Blit the Sysop logo PNG centred horizontally at the top.
4. `drawEyes()` — two small animated "eyes" in the logo that pulse in
   brightness using a slow sine-like ramp (`eye_color`, `eye_change`).
5. `drawFiles()` — file list overlay from `file_browser`.
6. `flip_buffers()` then `wait_hdmi_vblank()`.

**`update()`** — advances animation state: `updateStars()`, `advanceStars()`,
`updateEyes()`. Called at ~50 Hz from the main loop.

**Key globals (declared in display.h):**

| Symbol | Type | Purpose |
|--------|------|---------|
| `g_framebuffer_width/height` | `int` | 1920 × 1080 |
| `pFrameBuffer1/2` | `unsigned char *` | Raw pixel buffers |
| `g_cr1/2` | `cairo_t *` | Cairo contexts |
| `g_layout1/2` | `PangoLayout *` | Pango text layouts |
| `g_image` | `cairo_surface_t *` | Logo PNG |
| `framebuffer_visible` | `int` | 0 = hidden, 1 = showing |

---

### file_browser.cpp / file_browser.h

Handles directory traversal, D64 browsing, and drawing the file list.

**Root directory:** `g_root = "/mnt/data/c64_files"` — change this to point
at wherever C64 files are stored on the host.

**`fs_item` struct:**

| Field | Meaning |
|-------|---------|
| `fullpath` | Absolute host path |
| `name` | Display name |
| `d_type` | `dirent` type (`DT_DIR`, `DT_REG`, …) |
| `locationType` | `FileSystem` or `D64` |
| `parent` | For D64 entries: path to the `.d64` file |

**`get_items(path, list)`** — reads a host directory. `.d64` files are
recognised as containers; selecting one calls `getd64_items()` to list their
contents as virtual `D64`-type entries.

**`getd64_items(parent, path, list)`** — delegates to `get_items_from_d64()`
in `disk.cpp` to enumerate a D64 directory.

**Sorting:** `fs_item_sort_name` — case-insensitive alphabetical, `..` always
first.

**Scrolling:** `top_position` tracks the first visible row; `MAX_DRAW_ITEMS`
(20) is the page size. `show_items()` fills the display window; `update_position()`
highlights the cursor row.

**`drawFiles(cr, redraw_needed)`** — renders the visible slice of `g_file_list`
using Pango at monospace 20 pt. Only redraws when `redraw_needed != 0` to
avoid unnecessary Cairo calls.

---

### disk.cpp

Parses 1541 `.d64` disk images and loads files from them into C64 RAM via DMA.

**`get_items_from_d64(path, list)`** — opens a `.d64`, reads the BAM and
directory sectors (track 18), and returns a list of filenames. Handles the
standard 35-track layout; the `trackInfo[40][2]` table maps each track to its
sector count and byte offset within the image file.

**`d64_load(d64filename, filename)`** — locates `filename` in the D64 directory,
follows the track/sector chain, and DMA-writes the file data into C64 RAM
starting at the load address embedded in the first two bytes of the PRG.

**D64 constants used:**

| Constant | Value | Meaning |
|----------|-------|---------|
| `SECTOR_SIZE` | 256 | Bytes per sector |
| `DIRECTORY_TRACK` | 18 | Track holding the directory |
| `DIRECTORY_SECTOR` | 1 | First directory sector |
| `BAM_SECTOR` | 0 | Block Availability Map sector |

---

### keyboard.c

Scans the physical C64 keyboard matrix via the CIA1 registers, maps scan codes
to ASCII / special-key constants, and maintains a state table consumed by
`system_is_key_down()` and `system_is_shift_key_down()` in the main loop.

The 8×8 matrix (64 keys) is defined in `raw_keys[]` with each entry holding
the CIA row/column address, the unshifted character, and the shifted character.

**Key API (declared in `keyboard.h`):**

```c
void sysop_scan_keys(void);
int  sysop_is_key_down(int key);
int  sysop_is_shift_key_down(void);
int  is_button_pressed(int button);
```

`is_button_pressed()` polls a physical hardware button (button index 1 is used
by the outer loop to open/close the file browser).

---

### stars.c / stars.h

Self-contained 3-D starfield engine rendered with Cairo.

**80 stars** stored in `g_stars[]` as packed big-endian signed 16-bit triples
`(x, y, z)` per star (6 bytes each). The array is mutated in-place every
frame.

**Coordinate ranges:**

| Axis | Range | Wrap behaviour |
|------|-------|----------------|
| x | −384 .. +384 | wraps at ±`starfield_width` |
| y | −256 .. +256 | wraps at ±`starfield_height` |
| z | 64 .. 1024 | wraps with a 960-unit stride (keeps z > 0) |

**Velocity (`updateStars`):**  
Each axis reads one signed 16-bit value from `g_incTable` at its current
phase offset (`x/y/z_inc_offset`). The increment is doubled before being
applied, giving an effective speed range of ±16 units/frame.

**Wave table (`g_incTable`, 2048 bytes):**  
A triangle wave `0 → +8 → 0 → −8 → 0`, each step held for 32 pairs × 2 bytes
= 64 bytes per step, 32 steps total. Generated at startup by `initIncTable()`.
The three axes start at different offsets (980, 1492, 1748) so their
velocities are always out of phase, giving organic non-uniform motion.

**Projection (`drawStars`):**

```
screen_x = (star_x * 128 / z) * 3  +  center_x
screen_y = (star_y * 127 / z) * 3  +  center_y
```

The 3× magnification spreads stars across the full 1920×1080 display.

**Depth cueing:** `colorIndex = clamp(7 − floor(z / 128), 0, 7)`.  
Index 6 is white (nearest), index 0 is black (farthest/invisible). Colours are
in `g_starColors[]` (float RGB, easily changed for non-grayscale effects).

**Public API:**

```c
void initIncTable(void);  /* call once at startup */
void updateStars(void);   /* advance positions one frame */
void advanceStars(void);  /* step wave-table phase pointers */
void drawStars(cairo_t *cr, int width, int height, int setOrClear);
```

---

## Build

From the `tools/` directory:

```sh
make sysop_menu
```

The binary is placed in `build/sysop_menu`.

**Required libraries (ARM Linux target):**

| Library | Package (Debian/RPi) |
|---------|----------------------|
| Cairo | `libcairo2-dev` |
| Pango | `libpango1.0-dev` |
| GLib / GObject | `libglib2.0-dev` |
| libevdev | `libevdev-dev` |
| libsysop64 | built from `../libsysop64/` |

Header search paths for Cairo/Pango are hardcoded in the Makefile build rule
for ARM targets (`/usr/include/cairo`, `/usr/include/pango-1.0`, etc.).

---

## Runtime requirements

| Item | Details |
|------|---------|
| `/dev/sysop-fb` | HDMI framebuffer device (provided by the FPGA driver) |
| `/usr/local/bin/sysop-logo-480x196.png` | Logo PNG (must be present) |
| `/mnt/data/c64_files` | Root of the file browser (`g_root` in file_browser.cpp) |
| sysop server | Must be running before `sysop_menu` is started |

---

## Extending

**Change the file root:** Edit `g_root` at the top of `file_browser.cpp`.

**Add a new file type handler:** Add a branch in the `Return` key block in
`main.cpp` — check `item.locationType` and `item.name` extension, then call
into libsysop64 to load the data.

**Change star appearance:** Edit `g_starColors[]` in `stars.c` for colour, or
the `wave[]` array in `initIncTable()` for speed range.

**Change font / size:** `font_size` in `display.cpp` and the `pango_font_description_from_string`
calls in `main.cpp`.
