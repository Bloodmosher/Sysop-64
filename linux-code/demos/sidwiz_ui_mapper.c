/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * sidwiz_ui_mapper.c — SID-Wizard UI region map implementation.
 *
 * Maps 1920×1080 host mouse coordinates to the C64 memory writes needed
 * to move the SID-Wizard editor cursor.  All bounding-box constants were
 * measured empirically from a 1920×1080 HDMI capture of SID-Wizard.
 *
 * Coordinate system
 * -----------------
 * Origin (0,0) is the top-left corner of the 1920×1080 display.
 * The C64 active display area occupies roughly x 319–1601, y 122–1040,
 * leaving ~319 px borders on each side.  SID-Wizard fills the active area
 * with its editor views; the pixel regions below reflect that layout.
 *
 * Adding new regions
 * ------------------
 * 1. Measure the pixel bounding box from a screen capture.
 * 2. Add a get_*_rect() function returning that Rect.
 * 3. Add a point_in_rect() check in map_mouse_to_ui() and fill in the
 *    appropriate UIAction fields (see sidwiz_ui_mapper.h for field docs).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "sidwiz_ui_mapper.h"

/* ── Screen / character grid constants ─────────────────────────────────── */

#define SCREEN_WIDTH  1920
#define SCREEN_HEIGHT 1080

/* C64 active display: 40×25 characters scaled to fill the area inside the
 * HDMI borders.  CHAR_WIDTH and CHAR_HEIGHT give the pixel size of one
 * C64 character cell. */
#define BORDER_LEFT   240
#define BORDER_TOP    135
#define BORDER_RIGHT  240
#define BORDER_BOTTOM 135
#define C64_CHAR_WIDTH  40
#define C64_CHAR_HEIGHT 25
#define CHAR_WIDTH  ((SCREEN_WIDTH  - BORDER_LEFT - BORDER_RIGHT)  / C64_CHAR_WIDTH)
#define CHAR_HEIGHT ((SCREEN_HEIGHT - BORDER_TOP  - BORDER_BOTTOM) / C64_CHAR_HEIGHT)

/* Order list uses a narrower character grid */
#define ORDER_LIST_CHAR_WIDTH  32
#define ORDER_LIST_CHAR_HEIGHT 25

/* ── Region bounding boxes ──────────────────────────────────────────────── */

/*
 * get_pattern_track_rect — pixel bounds of one of the three pattern
 * editor tracks (track 0 = leftmost, track 2 = rightmost).
 */
Rect get_pattern_track_rect(int track)
{
    switch (track) {
    case 0: return (Rect){ 352,  122,  635, 916 };
    case 1: return (Rect){ 673,  122,  958, 916 };
    case 2: return (Rect){ 994,  122, 1276, 916 };
    default: return (Rect){ 0, 0, 0, 0 };
    }
}

Rect get_orderlist_rect(void)      { return (Rect){ 319, 956, 1601, 1040 }; }
Rect get_instrument_main_rect(void){ return (Rect){ 1346, 157, 1599,  281 }; }
Rect get_wfarp_rect(void)          { return (Rect){ 1314, 284, 1435,  569 }; }
Rect get_pulse_rect(void)          { return (Rect){ 1314, 570, 1435,  730 }; }
Rect get_filter_rect(void)         { return (Rect){ 1314, 731, 1435,  887 }; }
Rect get_chord_rect(void)          { return (Rect){ 1538, 316, 1594,  731 }; }
Rect get_tempo_rect(void)          { return (Rect){ 1538, 765, 1594,  888 }; }

/* ── Internal helpers ───────────────────────────────────────────────────── */

static int point_in_rect(Point p, Rect r)
{
    return p.x >= r.x1 && p.x < r.x2 && p.y >= r.y1 && p.y < r.y2;
}

/*
 * pixel_to_row_col — convert pixel coordinates within a region to a
 * (row, col) character-grid position.
 */
static void pixel_to_row_col(int px, int py,
                              int region_x, int region_y,
                              int col_w, int row_h,
                              int *out_col, int *out_row)
{
    *out_row = (py < region_y) ? 0 : (py - region_y) / row_h;
    *out_col = (px < region_x) ? 0 : (px - region_x) / col_w;
}

/*
 * pattern_pixel_to_row_col — like pixel_to_row_col but adjusts the column
 * calculation to account for the half-cell offset used in SID-Wizard's
 * pattern editor column layout.
 *
 * In the pattern editor each visible column is offset by half a character
 * cell, so we subtract col_w/2 from the x position before dividing.
 */
static void pattern_pixel_to_row_col(int px, int py,
                                     int region_x, int region_y,
                                     int col_w, int row_h,
                                     int *out_col, int *out_row)
{
    *out_row = (py < region_y) ? 0 : (py - region_y) / row_h;

    if (px < region_x) {
        *out_col = 0;
    } else if (px < region_x + col_w) {
        /* First column — no half-cell offset applied */
        *out_col = 0;
    } else {
        *out_col = (px - (col_w / 2) - region_x) / col_w;
    }
}

/* ── Main mapping function ──────────────────────────────────────────────── */

UIAction map_mouse_to_ui(int mouse_x, int mouse_y)
{
    UIAction action = {0};
    Point p = { mouse_x, mouse_y };

    /* ── Pattern editor (three tracks) ──────────────────────────────── */
    for (int track = 0; track < 3; track++) {
        Rect r = get_pattern_track_rect(track);
        if (!point_in_rect(p, r)) continue;

        action.curwind = CURWIND_PATTERN;

        int col, row;
        pattern_pixel_to_row_col(mouse_x, mouse_y,
                                  r.x1, r.y1,
                                  CHAR_WIDTH, CHAR_HEIGHT,
                                  &col, &row);

        /* Columns 0–3 = Note, 4–6 = Instrument, 7+ = Command */
        uint8_t subwindow = (col < 4) ? 0 : (col < 7) ? 1 : 2;

        action.subwindow_address = PATTERN_EDITOR_SUBWINDOW_ADDR;
        action.subwindow_value   = (uint8_t)track;
        action.row_address       = ADDR_PTNROW;
        action.row_value         = (uint8_t)row;
        action.column_address    = ADDR_PTNCOL;
        action.column_value      = (uint8_t)col;

        snprintf(action.description, sizeof(action.description),
                 "Pattern Track %d, Row %d, Column %d", track + 1, row, subwindow);
        return action;
    }

    /* ── Order list ──────────────────────────────────────────────────── */
    {
        Rect r = get_orderlist_rect();
        if (point_in_rect(p, r)) {
            action.curwind = CURWIND_ORDERLIST;

            int col, row;
            pixel_to_row_col(mouse_x, mouse_y,
                             r.x1, r.y1,
                             ORDER_LIST_CHAR_WIDTH, ORDER_LIST_CHAR_HEIGHT,
                             &col, &row);

            action.row_address    = 0x345;
            action.row_value      = (uint8_t)row;
            action.column_address = 0x34A;   /* Track selection */
            action.column_value   = (uint8_t)col;

            snprintf(action.description, sizeof(action.description),
                     "Order List, Row %d, Track %d", row, col);
            return action;
        }
    }

    /* ── Instrument main area ────────────────────────────────────────── */
    {
        Rect r = get_instrument_main_rect();
        if (point_in_rect(p, r)) {
            action.curwind = CURWIND_INSTRUM;

            int col, row;
            pixel_to_row_col(mouse_x, mouse_y,
                             r.x1, r.y1,
                             CHAR_WIDTH, CHAR_HEIGHT,
                             &col, &row);

            action.subwindow_address = 0x346;
            action.subwindow_value   = 0;
            action.row_address       = 0x361;
            action.row_value         = (uint8_t)row;
            action.column_address    = 0x35D;
            action.column_value      = (uint8_t)col;

            snprintf(action.description, sizeof(action.description),
                     "Instrument Editor, Row %d, Col %d", row, col);
            return action;
        }
    }

    /* ── WFARP table ─────────────────────────────────────────────────── */
    {
        Rect r = get_wfarp_rect();
        if (point_in_rect(p, r)) {
            action.curwind = CURWIND_INSTRUM;

            int col, row;
            pixel_to_row_col(mouse_x, mouse_y,
                             r.x1, r.y1,
                             CHAR_WIDTH, CHAR_HEIGHT,
                             &col, &row);

            action.subwindow_address = 0x346;
            action.subwindow_value   = 1;
            action.row_address       = ADDR_WFARPROW;
            action.row_value         = (row > 0) ? (uint8_t)(row - 1) : 0;
            action.column_address    = ADDR_WFARPCOL;
            action.column_value      = (uint8_t)col;

            snprintf(action.description, sizeof(action.description),
                     "WFARP Table, Row %d, Col %d", action.row_value, col);
            return action;
        }
    }

    /* ── PULSE table ─────────────────────────────────────────────────── */
    {
        Rect r = get_pulse_rect();
        if (point_in_rect(p, r)) {
            action.curwind = CURWIND_INSTRUM;

            int col, row;
            pixel_to_row_col(mouse_x, mouse_y,
                             r.x1, r.y1,
                             CHAR_WIDTH, CHAR_HEIGHT,
                             &col, &row);

            action.subwindow_address = 0x346;
            action.subwindow_value   = 2;
            action.row_address       = 0x363;
            action.row_value         = (row > 0) ? (uint8_t)(row - 1) : 0;
            action.column_address    = 0x35F;
            action.column_value      = (uint8_t)col;

            snprintf(action.description, sizeof(action.description),
                     "Pulse Table, Row %d, Col %d", action.row_value, col);
            return action;
        }
    }

    /* ── FILTER table ────────────────────────────────────────────────── */
    {
        Rect r = get_filter_rect();
        if (point_in_rect(p, r)) {
            action.curwind = CURWIND_INSTRUM;

            int col, row;
            pixel_to_row_col(mouse_x, mouse_y,
                             r.x1, r.y1,
                             CHAR_WIDTH, CHAR_HEIGHT,
                             &col, &row);

            action.subwindow_address = 0x346;
            action.subwindow_value   = 3;
            action.row_address       = 0x364;
            action.row_value         = (row > 0) ? (uint8_t)(row - 1) : 0;
            action.column_address    = 0x360;
            action.column_value      = (uint8_t)col;

            snprintf(action.description, sizeof(action.description),
                     "Filter Table, Row %d, Col %d", action.row_value, col);
            return action;
        }
    }

    /* Click was outside all known regions */
    snprintf(action.description, sizeof(action.description),
             "Unknown region at (%d, %d)", mouse_x, mouse_y);
    return action;
}
