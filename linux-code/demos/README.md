# demos

Example programs that exercise various features of [libsysop64](../libsysop64/README.md).

Project home: https://github.com/Bloodmosher/Sysop-64

---

## Building

```
make
```

Binaries and required assets are placed in `../build/demos/`.

`sid_visualizer` requires Cairo and its pkg-config metadata.  
`sidmidi` requires ALSA (`libasound`).

---

## Demos

### `hdmi_colors`
A collection of small palette and border experiments for the HDMI output path.  
Each sub-demo exercises a specific feature — per-scanline palette swaps, extended border modes, and colour ramp generation — and runs until a key is pressed.

### `bmlogo2`
Animated bitmap logo with a 3-D starfield background.  
A 320×200 single-colour bitmap (`bmlogo2.bin`) is split into 8 vertical sprite slices that sweep across the screen while an 80-star 3-D starfield is animated behind them in C64 hires-bitmap mode.

### `digiplay`
Raw digi sample playback through the SID volume register.  
Streams unsigned 8-bit PCM samples to the C64's `$D418` register at a selectable sample rate, using the SID DAC leak to produce audio output.

### `sidplayer`
SID file player.  
Loads a `.sid` file, prints title and author, then plays the selected subtune on the real C64 by writing SID register values every VBlank via the sysop bridge.

```
sidplayer <file.sid> [subtune]
```

### `sidmidi`
Virtual NameSoft MIDI keyboard for SID-Wizard on the C64.  
Bridges MIDI note data to SID-Wizard (or any NameSoft-compatible software) running on the C64.  Accepts input from the host keyboard, an ALSA MIDI device (`-d`), or a previously recorded `.mrec` file (`-p`).

### `anim_demo`
C64 character-set animation demo using Cairo-rendered frames.  
Loads pre-rendered 8 KB bitmap frames from `./frames/frame_NNNN.bin` and streams them to the C64's character-set RAM every VBlank using cycle-timed DMA writes, with per-line colour cycling applied via raster-exact `$D020`/`$D021` pokes.

```
anim_demo
# expects ./frames/ containing frame_0001.bin … frame_NNNN.bin
```

### `sid_visualizer`
Real-time SID voice waveform visualizer at 1920×1080.  
A sampler thread captures all three SID voice outputs at 48 kHz into a 10-second ring buffer.  The render loop draws anti-aliased waveform traces to the Sysop-64 dual framebuffer every HDMI vblank.  Supports optional mouse input (pointer, settings menu, scroll wheel, and SID-Wizard editor click-mapping via `--ui-map`).

---

## Assets

| File | Used by |
|------|---------|
| `assets/bmlogo2.bin` | `bmlogo2` |
| `assets/blood_mosher.raw` | `bmlogo2` |
| `assets/mouse_pointer.png` | `sid_visualizer` (embedded at link time) |
