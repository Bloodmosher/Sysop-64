/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * sidwiz_ui_mapper.h — SID-Wizard editor UI region map for 1920×1080.
 *
 * This module translates host mouse coordinates (in pixels) to the C64
 * memory locations that SID-Wizard uses to track the editor cursor.
 * Writing those memory locations via the DMA bridge is equivalent to
 * clicking in the corresponding editor region on the C64.
 *
 * Screen layout assumed (SID-Wizard on HDMI at 1920×1080):
 *   Pattern editor:   three track columns, left half of screen
 *   Order list:       bottom bar across all three tracks
 *   Instrument editor: right side, top
 *   WFARP / PULSE / FILTER tables: right side, mid
 *   Chord / Tempo tables: far right column
 *
 * All pixel coordinates were measured empirically against a 1920×1080
 * HDMI capture of SID-Wizard and may need adjustment if the video
 * scaling or border settings change.
 */

#ifndef SIDWIZ_UI_MAPPER_H
#define SIDWIZ_UI_MAPPER_H

#include <stdint.h>

/* ── SID-Wizard C64 memory addresses ──────────────────────────────────── */

/* Editor window selection ($343) */
#define ADDR_CURWIND  0x343

/* Subwindow / scroll positions for each editor view */
#define ADDR_SUBWPOS1 0x344
#define ADDR_SUBWPOS2 0x349
#define ADDR_SUBWPOS3 0x34E

/* Pattern editor: first-visible-row per track (3 bytes) */
#define ADDR_PROWPOS  0x353

/* Pattern editor: cursor column per track (3 bytes) */
#define ADDR_PTNCURS  0x356

/* Pattern editor: display column per track (3 bytes) */
#define ADDR_PTNMPOS  0x359

/* Pattern editor row / column (aliases used in some contexts) */
#define ADDR_PTNROW   0x34E
#define ADDR_PTNCOL   0x349

/* Instrument editor internals */
#define ADDR_PTRDYSI  0x35C
#define ADDR_INSWXBUF 0x35D  /* 4 bytes */
#define ADDR_INSWYBUF 0x361  /* 2 bytes */

/* Table scroll positions */
#define ADDR_WFARPOS  0x362
#define ADDR_PWTBPOS  0x363
#define ADDR_CTFTPOS  0x364
#define ADDR_CHORPOS  0x365
#define ADDR_TEMPPOS  0x366

/* Waveform arpeggio table cursor */
#define ADDR_WFARPCOL 0x35E
#define ADDR_WFARPROW 0x362

/* Pattern editor subwindow register (selects which track is active) */
#define PATTERN_EDITOR_SUBWINDOW_ADDR 0x344

/* ── curwind values ────────────────────────────────────────────────────── */

#define CURWIND_PATTERN    0  /* Pattern / note editor */
#define CURWIND_ORDERLIST  1  /* Song order list */
#define CURWIND_INSTRUM    2  /* Instrument tables */
#define CURWIND_CHORD      3  /* Chord table */
#define CURWIND_TEMPO      4  /* Tempo table */

/* ── Types ─────────────────────────────────────────────────────────────── */

typedef struct { int x, y;          } Point;
typedef struct { int x1, y1, x2, y2; } Rect;

typedef struct {
    Rect bounds;
    int  curwind;
    int  subwindow;
} UIRegion;

/*
 * UIAction — the result of mapping a mouse click.
 *
 * The caller should write subwindow_value to subwindow_address, then
 * row_value to row_address, then column_value to column_address via
 * sysop_poke() to move the SID-Wizard cursor to the clicked position.
 * Any address field that is zero means "no write needed".
 */
typedef struct {
    int      curwind;
    uint16_t subwindow_address;
    uint16_t row_address;
    uint16_t column_address;
    uint8_t  subwindow_value;
    uint8_t  row_value;
    uint8_t  column_value;
    char     description[128];
} UIAction;

/* ── Region accessors (pixel bounding boxes at 1920×1080) ─────────────── */

Rect get_pattern_track_rect(int track);   /* track = 0, 1, or 2 */
Rect get_orderlist_rect(void);
Rect get_instrument_main_rect(void);
Rect get_wfarp_rect(void);
Rect get_pulse_rect(void);
Rect get_filter_rect(void);
Rect get_chord_rect(void);
Rect get_tempo_rect(void);

/* ── Main mapping function ─────────────────────────────────────────────── */

/*
 * map_mouse_to_ui — convert a mouse click at (mouse_x, mouse_y) into a
 * UIAction describing which SID-Wizard memory locations to update and
 * with what values.
 *
 * If the click falls outside all known regions the returned action has
 * all address fields zero and a human-readable description string.
 */
UIAction map_mouse_to_ui(int mouse_x, int mouse_y);

#endif /* SIDWIZ_UI_MAPPER_H */
