/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * c64busview — C64 bus logic analyser
 *
 * Captures or replays a sequence of raw C64 bus samples.  The FPGA
 * records one 64-bit sample per 50 MHz clock cycle.  The PAL C64 PHI2
 * clock runs at ~985 kHz, so each C64 bus cycle is covered by roughly
 * 50 consecutive FPGA samples, giving fine sub-PHI2 resolution.
 *
 * Input modes
 * -----------
 *   ram <count> [--trigger-sampler]
 *       Read <count> samples from the FPGA sample RAM.  By default the
 *       sampler is NOT re-triggered; use --trigger-sampler to arm it
 *       here and wait for the capture to complete first.  This allows
 *       the tool to be used after a capture was triggered externally.
 *
 *   <filename> <count>
 *       Decode <count> raw uint64_t words from a binary file saved
 *       previously (e.g. via dd from SYSOP64_SAMPLER_BUFFER_ADDRESS).  No
 *       hardware connection required.
 *
 * Output modes
 * ------------
 *   (default)   CSV on stdout — pipe to a file or grep/awk.
 *   --viewer    Interactive ncurses viewer in the current terminal with
 *               fixed column headers, keyboard navigation, and search.
 *
 * Viewer key bindings
 * -------------------
 *   j / Down     Move down one row
 *   k / Up       Move up one row
 *   h / Left     Move cursor one column left
 *   l / Right    Move cursor one column right
 *   Space / PgDn Page down
 *   b / PgUp     Page up
 *   g / Home     Jump to first sample
 *   G / End      Jump to last sample
 *   /            Search within the current column (substring, case-insensitive)
 *   n            Jump to next search match
 *   N            Jump to previous search match
 *   f            Set/update filter on current column (substring, case-insensitive)
 *   F            Clear filter on current column
 *   X            Clear all column filters
 *   Tab          Jump to next row that passes all active filters
 *   Shift+Tab    Jump to previous row that passes all active filters
 *   Esc          Clear search string (if active), otherwise quit
 *   q            Quit
 *
 * CSV columns: Sample, Tick, Line, Cyc, Ph2, BA, RW, Addr, Data,
 *              IRQ, DMA, Frz, RL, RH, I1, I2
 *
 * Saved file format
 * -----------------
 *   Each sample is one little-endian uint64_t (8 bytes).
 *   Save the FPGA sample RAM with:
 *     dd if=/dev/mem bs=8 count=<N> skip=$((0x21000000/8)) of=capture.bin
 *   (SYSOP64_SAMPLER_BUFFER_ADDRESS = 0x21000000 — see sysop_defines.h)
 */

#define _GNU_SOURCE     /* for strcasecmp / strncasecmp */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <ncurses.h>

#include "sysop64.h"
#include "sysop_defines.h"

/* ------------------------------------------------------------------ */
/* Raw bit-field decoder (used for file mode)                         */
/* ------------------------------------------------------------------ */

/*
 * decode_sample_raw — unpack a single 64-bit FPGA bus-sample word into
 * the standard sysop_c64_bus_sample struct.
 *
 * The FPGA concatenates C64 bus signals into one 64-bit word each clock
 * cycle.  Bit layout (bit 0 = LSB):
 *
 *   [7:0]   data          — C64 data bus
 *   [23:8]  addr          — address bus A15..A0
 *   [24]    r__w          — R/_W line  (1 = read, 0 = CPU write)
 *   [25]    ba            — Bus Available
 *   [26]    phi2          — PHI2 clock
 *   [27]    _irq          — _IRQ (active-low)
 *   [28]    _dma          — _DMA
 *   [29]    freeze_signal — FPGA freeze output
 *   [37:30] cycle         — VIC-II character cycle within the raster line
 *   [46:38] vic_line      — VIC-II raster line (9 bits; 0–311 PAL)
 *   [52:47] sample_tick   — sub-PHI2 tick counter (6 bits)
 *   [57:53] (reserved)    — FPGA-internal sample-point flags (CPU write
 *                           window, PHI hi/lo points, data-valid, colour-
 *                           RAM write-enable).  Hardware-version specific;
 *                           not decoded here but present in s->raw.
 *   [58]    _io2          — _IO2 ($DF00 range)
 *   [59]    _io1          — _IO1 ($DE00 range)
 *   [60]    _romh         — _ROMH
 *   [61]    _roml         — _ROML
 *   [62]    _nmi          — _NMI (not in struct; inspect s->raw if needed)
 *   [63]    (unused)
 *
 * This mirrors the decoding performed by the library's sysop_sampler_get_sample(),
 * allowing binary captures to be decoded offline without a hardware
 * connection.
 *
 * Note: _charen, _hiram, _loram, _exrom, _game are not populated here
 * because their bit positions conflict with the sample-point flags in the
 * current FPGA revision.  Inspect s->raw directly if you need them.
 */
static void decode_sample_raw(uint64_t raw, struct sysop_c64_bus_sample *s)
{
    s->raw           = raw;
    s->data          = (uint8_t)(raw & 0xFF);
    s->addr          = (uint16_t)((raw >> 8) & 0xFFFF);
    s->r__w          = (uint8_t)((raw >> 24) & 1);
    s->ba            = (uint8_t)((raw >> 25) & 1);
    s->phi2          = (uint8_t)((raw >> 26) & 1);
    s->_irq          = (uint8_t)((raw >> 27) & 1);
    s->_dma          = (uint8_t)((raw >> 28) & 1);
    s->freeze_signal = (uint8_t)((raw >> 29) & 1);
    s->cycle         = (uint8_t)((raw >> 30) & 0xFF);
    s->vic_line      = (uint16_t)((raw >> 38) & 0x1FF);
    s->sample_tick   = (uint32_t)((raw >> 47) & 0x3F);
    s->_io2          = (uint8_t)((raw >> 58) & 1);
    s->_io1          = (uint8_t)((raw >> 59) & 1);
    s->_romh         = (uint8_t)((raw >> 60) & 1);
    s->_roml         = (uint8_t)((raw >> 61) & 1);

    /* Not decoded — see comment above */
    s->_charen = 0;
    s->_hiram  = 0;
    s->_loram  = 0;
    s->_exrom  = 0;
    s->_game   = 0;
}

/* ------------------------------------------------------------------ */
/* Column definitions                                                  */
/* ------------------------------------------------------------------ */

/*
 * Each column has a header label and a fixed display width (characters).
 * The total across all 16 columns plus 15 '│' separators is 66 chars,
 * comfortably fitting an 80-column terminal.
 *
 *  Col  Header  Width  Format
 *  ---  ------  -----  ------
 *   0   Sample    8    %8u
 *   1   Tick      4    %4u   (0-63, sub-PHI2 tick)
 *   2   Line      4    %4u   (0-311 PAL raster line)
 *   3   Cyc       3    %3u   (VIC-II character cycle)
 *   4   Ph2       3    %3u   (PHI2 clock state)
 *   5   BA        2    %2u   (Bus Available)
 *   6   RW        2    " R"/" W"
 *   7   Addr      4    %04X
 *   8   Data      4    %4X   (upper 2 digits always 0 for 8-bit data)
 *   9   IRQ       3    %3u
 *  10   DMA       3    %3u
 *  11   Frz       3    %3u
 *  12   RL        2    %2u   (_ROML)
 *  13   RH        2    %2u   (_ROMH)
 *  14   I1        2    %2u   (_IO1)
 *  15   I2        2    %2u   (_IO2)
 *  16   CH        2    %2u   (_CHAREN)
 *  17   HI        2    %2u   (_HIRAM)
 *  18   LO        2    %2u   (_LORAM)
 *  19   EX        2    %2u   (_EXROM)
 *  20   GM        2    %2u   (_GAME)
 *
 * Note: CH/HI/LO/EX/GM are populated by sysop_sampler_get_sample() (RAM mode).
 * They are zeroed by the offline decode_sample_raw() used in file mode.
 */

#define NUM_COLUMNS  21
#define LOCKED_COLS   4     /* Sample, Tick, Line, Cyc — always visible */
#define MAX_SEARCH   48     /* max search string length */
#define MAX_FILTER   48     /* max filter string length per column */
#define SCAN_PROGRESS_INTERVAL 50000  /* show progress every N samples during long scans */
#define RENDER_ROW_LOOKAHEAD  500000  /* max samples to scan between displayed filter rows */

typedef struct { const char *header; int width; } ColumnDef;

static const ColumnDef COLUMNS[NUM_COLUMNS] = {
    { "Sample", 8 }, { "Tick", 4 }, { "Line", 4 }, { "Cyc", 3 },
    { "Ph2",    3 }, { "BA",   2 }, { "RW",   2 }, { "Addr", 4 },
    { "Data",   4 }, { "IRQ",  3 }, { "DMA",  3 }, { "Frz",  3 },
    { "RL",     2 }, { "RH",   2 }, { "I1",   2 }, { "I2",   2 },
    { "CH",     2 }, { "HI",   2 }, { "LO",   2 }, { "EX",   2 },
    { "GM",     2 },
};

/* Format the value of column `col` for sample `idx` into `buf`. */
static void get_cell(char *buf, int col, uint32_t idx,
                     const struct sysop_c64_bus_sample *s)
{
    switch (col) {
    case  0: snprintf(buf, 32, "%8u",  idx);                      break;
    case  1: snprintf(buf, 32, "%4u",  s->sample_tick);           break;
    case  2: snprintf(buf, 32, "%4u",  (unsigned)s->vic_line);    break;
    case  3: snprintf(buf, 32, "%3u",  (unsigned)s->cycle);       break;
    case  4: snprintf(buf, 32, "%3u",  s->phi2);                  break;
    case  5: snprintf(buf, 32, "%2u",  s->ba);                    break;
    case  6: snprintf(buf, 32, " %c",  s->r__w ? 'R' : 'W');     break;
    case  7: snprintf(buf, 32, "%04X", s->addr);                  break;
    case  8: snprintf(buf, 32, "  %02X", s->data);                break;
    case  9: snprintf(buf, 32, "%3u",  s->_irq);                  break;
    case 10: snprintf(buf, 32, "%3u",  s->_dma);                  break;
    case 11: snprintf(buf, 32, "%3u",  s->freeze_signal);         break;
    case 12: snprintf(buf, 32, "%2u",  s->_roml);                 break;
    case 13: snprintf(buf, 32, "%2u",  s->_romh);                 break;
    case 14: snprintf(buf, 32, "%2u",  s->_io1);                  break;
    case 15: snprintf(buf, 32, "%2u",  s->_io2);                  break;
    case 16: snprintf(buf, 32, "%2u",  s->_charen);               break;
    case 17: snprintf(buf, 32, "%2u",  s->_hiram);                break;
    case 18: snprintf(buf, 32, "%2u",  s->_loram);                break;
    case 19: snprintf(buf, 32, "%2u",  s->_exrom);                break;
    case 20: snprintf(buf, 32, "%2u",  s->_game);                 break;
    default: buf[0] = '\0';                                        break;
    }
}

/* ------------------------------------------------------------------ */
/* CSV output helpers                                                  */
/* ------------------------------------------------------------------ */

static void print_csv_header(int prefix)
{
    if (prefix)
        printf("Sample,Tick,Line,Cycle,Phi2,BA,RW,Addr,Data,IRQ,DMA,Freeze,RomL,RomH,Io1,Io2,Charen,Hiram,Loram,Exrom,Game\n");
    else
        printf("Sample,Tick,Line,Cycle,Phi2,BA,RW,Addr,Data,IRQ,DMA,Freeze,RomL,RomH,Io1,Io2,Charen,Hiram,Loram,Exrom,Game\n");
}

static void print_csv_row(uint32_t index, const struct sysop_c64_bus_sample *s, int prefix)
{
    if (prefix) {
        printf("%u,%u,L%u,CYC%u,PHI2=%u,BA=%u,%c,%04X,%02X,IRQ=%u,DMA=%u,FREEZE=%u,RL=%u,RH=%u,IO1=%u,IO2=%u,CH=%u,HI=%u,LO=%u,EX=%u,GM=%u\n",
               index,
               s->sample_tick, (unsigned)s->vic_line, (unsigned)s->cycle,
               s->phi2, s->ba, s->r__w ? 'R' : 'W', s->addr, s->data,
               s->_irq, s->_dma, s->freeze_signal,
               s->_roml, s->_romh, s->_io1, s->_io2,
               s->_charen, s->_hiram, s->_loram, s->_exrom, s->_game);
    } else {
        printf("%u,%u,%u,%u,%u,%u,%u,%04X,%02X,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
               index,
               s->sample_tick, (unsigned)s->vic_line, (unsigned)s->cycle,
               s->phi2, s->ba, s->r__w, s->addr, s->data,
               s->_irq, s->_dma, s->freeze_signal,
               s->_roml, s->_romh, s->_io1, s->_io2,
               s->_charen, s->_hiram, s->_loram, s->_exrom, s->_game);
    }
}

/* ================================================================== */
/* Interactive ncurses viewer                                          */
/* ================================================================== */

/* ncurses color-pair indices */
#define CP_NORMAL      1    /* normal data row */
#define CP_HEADER      2    /* title and column header bar */
#define CP_CURSOR      3    /* cursor (selected) row */
#define CP_CURCOL      4    /* currently selected column header */
#define CP_WRITE       5    /* CPU write cycle rows (R/W = 0) */
#define CP_MATCH       6    /* search-match cell highlight */
#define CP_LOCK_SEP    7    /* separator between locked and scrollable panes */
#define CP_FILTER_COL  8    /* column header with an active filter */
#define CP_FILTER_BAR  9    /* filter status bar */
#define CP_DIMMED        10    /* row that does not pass active filters */
#define CP_PHI2_ROW      11    /* PHI2 waveform visualisation row */
#define CP_CURCOL_SHADE  12    /* non-cursor cells in the selected column */

/* ------------------------------------------------------------------ */
/* Sample source abstraction                                           */
/* ------------------------------------------------------------------ */

/*
 * SampleSource wraps two backends without allocating a sample buffer:
 *
 *   SRC_RAM  — FPGA sampler RAM; each call to source_get() invokes
 *              sysop_sampler_get_sample() which reads directly from the
 *              memory-mapped FPGA region.
 *
 *   SRC_FILE — binary capture file; every sample is exactly one
 *              little-endian uint64_t (8 bytes), so sample N lives at
 *              byte offset N*8.  source_get() seeks to the offset and
 *              reads one word.  No index table is needed.
 */
typedef enum { SRC_RAM, SRC_FILE } SrcType;

typedef struct {
    SrcType  type;
    FILE    *fp;        /* SRC_FILE: open file handle */
    uint32_t count;     /* total samples available */
} SampleSource;

static void source_get(SampleSource *src, uint32_t idx,
                        struct sysop_c64_bus_sample *s)
{
    if (src->type == SRC_RAM) {
        sysop_sampler_get_sample(idx, s);
    } else {
        uint64_t raw = 0;
        fseek(src->fp, (long)idx * (long)sizeof(uint64_t), SEEK_SET);
        if (fread(&raw, sizeof(raw), 1, src->fp) < 1)
            raw = 0;    /* past EOF — return a zeroed sample */
        decode_sample_raw(raw, s);
    }
}

/*
 * glob_match — case-insensitive glob match where '*' matches any sequence
 * of characters (including empty).  Used by cell_matches when the pattern
 * contains a wildcard.
 */
static int glob_match(const char *pat, const char *str)
{
    while (*pat && *str) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;           /* trailing '*' matches rest */
            while (*str) {
                if (glob_match(pat, str)) return 1;
                str++;
            }
            return 0;
        }
        if (*pat == '.') {                 /* '.' matches any single char */
            pat++; str++;
            continue;
        }
        if (tolower((unsigned char)*pat) != tolower((unsigned char)*str))
            return 0;
        pat++; str++;
    }
    while (*pat == '*') pat++;
    return (*pat == '\0') && (*str == '\0');
}

/*
 * cell_matches — test whether a formatted cell value matches `pattern`.
 *
 * Leading and trailing spaces are always stripped from the cell before
 * comparison.
 *
 * If `pattern` contains no '*': exact case-insensitive match against the
 * trimmed cell value.
 *
 * If `pattern` contains '*': case-insensitive glob match against the
 * trimmed cell value ('*' matches any sequence of characters).
 *
 * Examples (Line column, format "%4u"):
 *   pattern "1"    matches "   1"  (trimmed to "1")  but NOT "198" or "310"
 *   pattern "1*"   matches "1", "10", "198", "1xx"
 *   pattern "*1"   matches "1", "21", "101"
 *   pattern "*1*"  matches any cell whose trimmed value contains "1"
 */
static int cell_matches(const char *cell, const char *pattern)
{
    /* Trim leading spaces */
    while (*cell == ' ') cell++;
    /* Trim trailing spaces into a local buffer */
    char trimmed[34];
    int len = (int)strlen(cell);
    while (len > 0 && cell[len - 1] == ' ') len--;
    if (len >= (int)sizeof(trimmed)) len = (int)sizeof(trimmed) - 1;
    memcpy(trimmed, cell, (size_t)len);
    trimmed[len] = '\0';

    if (strchr(pattern, '*') || strchr(pattern, '.'))
        return glob_match(pattern, trimmed);
    return strcasecmp(trimmed, pattern) == 0;
}

/*
 * draw_scan_progress — update the status bar with a scanning progress message.
 * Called periodically during long search/filter scans so the user knows the
 * viewer is working.  Esc cancels (handled by the caller via nodelay+getch).
 */
static void draw_scan_progress(const char *op, int current, int total)
{
    move(LINES - 1, 0);
    attron(A_REVERSE | A_BOLD);
    printw(" %s...  %d / %d    Esc: cancel", op, current, total);
    clrtoeol();
    attroff(A_REVERSE | A_BOLD);
    refresh();
}

/*
 * search_next — scan for the next row whose current-column cell matches
 * `str` using cell_matches() (exact by default; '*' enables glob wildcards).
 *
 * Starts searching from `from + dir` (so passing cur_row with dir=1
 * finds the next match AFTER the cursor, and dir=-1 finds the one
 * BEFORE it).
 *
 * Returns the matching row index, or -1 if nothing is found.
 */
static int search_next(SampleSource *src, int col, int from, int dir,
                        const char *str, int *cancelled)
{
    if (cancelled) *cancelled = 0;
    char buf[32];
    struct sysop_c64_bus_sample s;
    int i = from + dir;
    long steps = 0;
    while (i >= 0 && i < (int)src->count) {
        source_get(src, (uint32_t)i, &s);
        get_cell(buf, col, (uint32_t)i, &s);
        if (cell_matches(buf, str))
            return i;
        i += dir;
        if (++steps % SCAN_PROGRESS_INTERVAL == 0) {
            draw_scan_progress("Searching", i, (int)src->count);
            nodelay(stdscr, TRUE);
            int kc = getch();
            nodelay(stdscr, FALSE);
            if (kc == 27) {
                if (cancelled) *cancelled = 1;
                return -1;
            } else if (kc != ERR) {
                ungetch(kc);
            }
        }
    }
    return -1;
}

/*
 * Fast-path filter support.
 *
 * For numeric/hex columns a simple integer equality check replaces the
 * snprintf + strcasecmp path.  The filter string is parsed once before a
 * scan loop begins; each per-sample check is then a single comparison.
 *
 * Columns that use non-numeric formatting (col 6 = R/W printed as 'R'/'W')
 * or that contain glob wildcards always fall back to the string path.
 */
typedef struct {
    int  valid;   /* 1 = fast integer compare available */
    long value;   /* parsed numeric value of the filter string */
} FilterFast;

/* Return the raw numeric field value for column `col`. */
static long field_value(int col, uint32_t idx, const struct sysop_c64_bus_sample *s)
{
    switch (col) {
    case  0: return (long)idx;
    case  1: return (long)s->sample_tick;
    case  2: return (long)(unsigned)s->vic_line;
    case  3: return (long)(unsigned)s->cycle;
    case  4: return (long)s->phi2;
    case  5: return (long)s->ba;
    /* col 6 = R/W: formatted as 'R'/'W', no numeric fast path */
    case  7: return (long)s->addr;
    case  8: return (long)s->data;
    case  9: return (long)s->_irq;
    case 10: return (long)s->_dma;
    case 11: return (long)s->freeze_signal;
    case 12: return (long)s->_roml;
    case 13: return (long)s->_romh;
    case 14: return (long)s->_io1;
    case 15: return (long)s->_io2;
    case 16: return (long)s->_charen;
    case 17: return (long)s->_hiram;
    case 18: return (long)s->_loram;
    case 19: return (long)s->_exrom;
    case 20: return (long)s->_game;
    default: return -1;
    }
}

/* Pre-parse all active filter strings into FilterFast entries.
 * Call once before a scan loop to avoid snprintf inside the hot path. */
static void compute_filter_fast(char filter_str[][MAX_FILTER + 1],
                                 FilterFast fast[])
{
    for (int c = 0; c < NUM_COLUMNS; c++) {
        fast[c].valid = 0;
        const char *f = filter_str[c];
        if (!f[0]) continue;
        if (strchr(f, '*') || strchr(f, '.')) continue; /* wildcard: must use string path */
        if (c == 6) continue;            /* R/W uses letter values */
        int base = (c == 7 || c == 8) ? 16 : 10;  /* Addr/Data are hex */
        while (*f == ' ') f++;           /* skip leading spaces */
        if (!*f) continue;
        char *end;
        long val = strtol(f, &end, base);
        while (*end == ' ') end++;       /* skip trailing spaces */
        if (end == f || *end != '\0') continue;
        fast[c].value = val;
        fast[c].valid = 1;
    }
}

/*
 * row_passes_filters — returns 1 if the sample passes all active column
 * filters.  Each filter uses cell_matches(): exact match by default,
 * glob wildcard when the filter string contains '*'.
 */
static int row_passes_filters(uint32_t idx, const struct sysop_c64_bus_sample *s,
                               char filter_str[][MAX_FILTER + 1])
{
    char buf[32];
    for (int c = 0; c < NUM_COLUMNS; c++) {
        if (!filter_str[c][0]) continue;
        get_cell(buf, c, idx, s);
        if (!cell_matches(buf, filter_str[c]))
            return 0;
    }
    return 1;
}

/* row_passes_filters_fast — hot-loop variant using pre-computed FilterFast.
 * Falls back to the snprintf path for columns where fast[] is not valid. */
static int row_passes_filters_fast(uint32_t idx,
                                    const struct sysop_c64_bus_sample *s,
                                    char filter_str[][MAX_FILTER + 1],
                                    const FilterFast fast[])
{
    char buf[32];
    for (int c = 0; c < NUM_COLUMNS; c++) {
        if (!filter_str[c][0]) continue;
        if (fast[c].valid) {
            if (field_value(c, idx, s) != fast[c].value) return 0;
        } else {
            get_cell(buf, c, idx, s);
            if (!cell_matches(buf, filter_str[c])) return 0;
        }
    }
    return 1;
}

/*
 * filter_next — scan for the next row (from + dir) that passes all active
 * column filters.  Returns the row index, or -1 if nothing matches.
 */
static int filter_next(SampleSource *src, int from, int dir,
                        char filter_str[][MAX_FILTER + 1], int *cancelled)
{
    if (cancelled) *cancelled = 0;
    /* Quick check: is any filter active? */
    int any = 0;
    for (int c = 0; c < NUM_COLUMNS; c++)
        if (filter_str[c][0]) { any = 1; break; }
    if (!any) return -1;

    FilterFast fast[NUM_COLUMNS];
    compute_filter_fast(filter_str, fast);

    struct sysop_c64_bus_sample s;
    int i = from + dir;
    long steps = 0;
    while (i >= 0 && i < (int)src->count) {
        source_get(src, (uint32_t)i, &s);
        if (row_passes_filters_fast((uint32_t)i, &s, filter_str, fast))
            return i;
        i += dir;
        if (++steps % SCAN_PROGRESS_INTERVAL == 0) {
            draw_scan_progress("Scanning", i, (int)src->count);
            nodelay(stdscr, TRUE);
            int kc = getch();
            nodelay(stdscr, FALSE);
            if (kc == 27) {
                if (cancelled) *cancelled = 1;
                return -1;
            } else if (kc != ERR) {
                ungetch(kc);
            }
        }
    }
    return -1;
}


/* ================================================================== */
/* VIC-II type detection and frame indexing                           */
/* ================================================================== */

#define VIC_UNKNOWN   (-1)
#define VIC_PAL         0   /* 6569:       63 cycles/line, 312 lines */
#define VIC_NTSC_OLD    1   /* 6567R56A:   64 cycles/line, 262 lines */
#define VIC_NTSC_NEW    2   /* 6567R8:     65 cycles/line, 263 lines */
#define VIC_DREAN       3   /* 6572 PAL-N: 65 cycles/line, 312 lines */

/* Stop scanning once this many frame starts have been confirmed.
 * 3 wraps gives 2 inter-frame intervals for an accurate period estimate
 * and is enough to determine any VIC variant; typically ~2–3 M samples. */
#define DETECT_LINE_WRAPS  3

#define VIC_MAX_FRAMES  64  /* generous upper bound for any capture   */

typedef struct {
    int      type;
    int      lines_per_frame;
    int      cycles_per_line;
    uint32_t frame_starts[VIC_MAX_FRAMES]; /* sample idx of each L=0,C=1 edge */
    int      frame_count;
    uint32_t avg_frame_len;  /* mean samples between successive frame starts */
    int      total_frames;   /* estimated total frames in full capture       */
} VicInfo;

static const char *vic_type_name(const VicInfo *vi)
{
    switch (vi->type) {
    case VIC_PAL:      return "PAL";
    case VIC_NTSC_OLD: return "NTSC-OLD";
    case VIC_NTSC_NEW: return "NTSC";
    case VIC_DREAN:    return "DREAN";
    default:           return "?";
    }
}

/*
 * detect_vic_info — scan just enough of the source to infer VIC type.
 * Stops as soon as DETECT_LINE_WRAPS frame starts have been observed
 * (typically ~2–3 M samples out of 12.5 M).  For file sources uses bulk
 * fread; for RAM sources uses source_get() on the mmap'd buffer.
 *
 * After the early-stop scan, avg_frame_len is estimated from the
 * collected frame_starts and total_frames is extrapolated over the full
 * capture so that get_frame_number() can work for any sample index.
 */
static void detect_vic_info(SampleSource *src, VicInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->type = VIC_UNKNOWN;
    uint16_t max_line  = 0;
    uint8_t  max_cycle = 0;
    int      prev_at_start = 0;

    if (src->type == SRC_FILE) {
        long saved = ftell(src->fp);
        fseek(src->fp, 0, SEEK_SET);
        enum { CHUNK = 8192 };
        uint64_t buf[CHUNK];
        uint32_t total = 0;
        int done = 0;
        size_t n;
        while (!done && total < src->count &&
               (n = fread(buf, sizeof(uint64_t), CHUNK, src->fp)) > 0) {
            if (n > (size_t)(src->count - total)) n = (size_t)(src->count - total);
            for (size_t i = 0; i < n; i++, total++) {
                uint64_t raw   = buf[i];
                uint16_t line  = (uint16_t)((raw >> 38) & 0x1FF);
                uint8_t  cycle = (uint8_t) ((raw >> 30) & 0xFF);
                if (line  > max_line)  max_line  = line;
                if (cycle > max_cycle) max_cycle = cycle;
                int at_start = (line == 0 && cycle == 1);
                if (at_start && !prev_at_start) {
                    if (info->frame_count < VIC_MAX_FRAMES)
                        info->frame_starts[info->frame_count++] = total;
                    if (info->frame_count >= DETECT_LINE_WRAPS)
                        { done = 1; break; }
                }
                prev_at_start = at_start;
            }
        }
        fseek(src->fp, saved, SEEK_SET);
    } else {
        struct sysop_c64_bus_sample s;
        for (uint32_t i = 0; i < src->count; i++) {
            source_get(src, i, &s);
            uint16_t line  = (uint16_t)(unsigned)s.vic_line;
            uint8_t  cycle = s.cycle;
            if (line  > max_line)  max_line  = line;
            if (cycle > max_cycle) max_cycle = cycle;
            int at_start = (line == 0 && cycle == 1);
            if (at_start && !prev_at_start) {
                if (info->frame_count < VIC_MAX_FRAMES)
                    info->frame_starts[info->frame_count++] = i;
                if (info->frame_count >= DETECT_LINE_WRAPS)
                    break;
            }
            prev_at_start = at_start;
        }
    }

    /* Determine VIC type from peak observed line and cycle values */
    if (max_line >= 311) {
        if (max_cycle >= 65) { info->type = VIC_DREAN;    info->lines_per_frame = 312; info->cycles_per_line = 65; }
        else                 { info->type = VIC_PAL;      info->lines_per_frame = 312; info->cycles_per_line = 63; }
    } else if (max_line >= 260) {
        if (max_cycle >= 65) { info->type = VIC_NTSC_NEW; info->lines_per_frame = 263; info->cycles_per_line = 65; }
        else                 { info->type = VIC_NTSC_OLD; info->lines_per_frame = 262; info->cycles_per_line = 64; }
    } else {
        info->type = VIC_PAL; info->lines_per_frame = 312; info->cycles_per_line = 63;
    }

    /* Compute average frame length and extrapolate total frame count */
    if (info->frame_count >= 2) {
        info->avg_frame_len = (info->frame_starts[info->frame_count - 1]
                               - info->frame_starts[0])
                              / (uint32_t)(info->frame_count - 1);
    }
    if (info->avg_frame_len > 0 && info->frame_count > 0) {
        uint32_t remaining = src->count > info->frame_starts[info->frame_count - 1]
                           ? src->count - info->frame_starts[info->frame_count - 1]
                           : 0;
        info->total_frames = info->frame_count
                           + (int)(remaining / info->avg_frame_len);
    } else {
        info->total_frames = info->frame_count;
    }
}

/*
 * get_frame_number — return the 0-based frame index for sample `idx`.
 * Uses binary search within the detected frame_starts[], then extrapolates
 * beyond the last detected boundary using avg_frame_len.
 * Returns -1 when no frame boundaries were found.
 */
static int get_frame_number(const VicInfo *info, uint32_t idx)
{
    if (info->frame_count == 0) return -1;

    /* Binary search for the last frame_start <= idx */
    int lo = 0, hi = info->frame_count - 1, best = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (info->frame_starts[mid] <= idx) { best = mid; lo = mid + 1; }
        else                                  hi = mid - 1;
    }

    if (idx < info->frame_starts[best]) return -1; /* before first boundary */

    /* Extrapolate beyond the last detected boundary */
    if (best == info->frame_count - 1 && info->avg_frame_len > 0) {
        uint32_t offset = idx - info->frame_starts[best];
        return best + (int)(offset / info->avg_frame_len);
    }
    return best;
}

/* ================================================================== */
/* Initialise ncurses color pairs.  Falls back gracefully if the      */
/* terminal reports no color support.                                  */
/* ================================================================== */
static void viewer_init_colors(void)
{
    if (!has_colors()) return;
    start_color();
    use_default_colors();   /* allows -1 = terminal's own default color */
    init_pair(CP_NORMAL, -1,           -1);
    init_pair(CP_HEADER, COLOR_WHITE,  COLOR_BLUE);
    init_pair(CP_CURSOR, COLOR_WHITE,  COLOR_BLUE);
    init_pair(CP_CURCOL, COLOR_YELLOW, COLOR_BLUE);
    init_pair(CP_WRITE,    COLOR_YELLOW, -1);
    init_pair(CP_MATCH,    COLOR_BLACK,  COLOR_YELLOW);
    /* Yellow-on-cyan makes the lock separator visible against every row
     * style: default rows, cursor (blue bg), write (yellow fg), header. */
    init_pair(CP_LOCK_SEP,   COLOR_YELLOW, COLOR_BLUE);
    init_pair(CP_FILTER_COL, COLOR_BLACK,  COLOR_YELLOW);
    init_pair(CP_FILTER_BAR, COLOR_BLACK,  COLOR_GREEN);
    init_pair(CP_DIMMED,       COLOR_WHITE,  -1);
    init_pair(CP_PHI2_ROW,     COLOR_CYAN,   -1);
    init_pair(CP_CURCOL_SHADE, COLOR_WHITE,  COLOR_BLUE);
}

/* Draw the top title bar (row 0). */
static void draw_title(uint32_t count, int is_ram, const VicInfo *vic_info)
{
    const char *hints = is_ram
        ? "q:quit  g/G:top/end  PgUp/Dn  </>:col  /:search  n/N  f:filter  Tab:jump  T:retrigger  V:detect "
        : "q:quit  g/G:top/end  PgUp/Dn  </>:col  /:search  n/N  f:filter  Tab:jump  V:detect ";

    attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
    move(0, 0);
    clrtoeol();

    if (vic_info->type != VIC_UNKNOWN)
        mvprintw(0, 0, " c64busview  %u samples  [%s %dL x %dC]",
                 count, vic_type_name(vic_info),
                 vic_info->lines_per_frame, vic_info->cycles_per_line);
    else
        mvprintw(0, 0, " c64busview  %u samples", count);

    int hx = COLS - (int)strlen(hints);
    if (hx > 0) mvprintw(0, hx, "%s", hints);

    attroff(A_BOLD);
}

/*
 * draw_phi2_waveform — draw a one-cycle PHI2 square-wave visualisation at
 * screen row `row`.
 *
 * Scans ±75 samples around `cur_idx` to build a tick→phi2 map for all
 * 64 possible sample_tick values (0–63).  Each tick maps to one character:
 *
 *   '_'  PHI2 low (sustained)
 *   '-'  PHI2 high (sustained)
 *   '/'  rising edge (previous tick was low)
 *   '\'  falling edge (previous tick was high)
 *   '*'  current cursor position (bold + reverse video)
 *   '.'  no sample observed at this tick within the scan window
 */
static void draw_phi2_waveform(int row, uint32_t cur_idx, SampleSource *src)
{
    int8_t tmap[64];
    memset(tmap, -1, sizeof(tmap));

    int32_t scan_start = (int32_t)cur_idx - 75;
    if (scan_start < 0) scan_start = 0;
    int32_t scan_end   = (int32_t)cur_idx + 75;
    if (scan_end >= (int32_t)src->count) scan_end = (int32_t)src->count - 1;

    struct sysop_c64_bus_sample s;
    for (int32_t i = scan_start; i <= scan_end; i++) {
        source_get(src, (uint32_t)i, &s);
        int t = (int)(s.sample_tick & 0x3F);
        tmap[t] = (int8_t)s.phi2;
    }

    source_get(src, cur_idx, &s);
    int cur_tick = (int)(s.sample_tick & 0x3F);

    move(row, 0);
    attron(COLOR_PAIR(CP_PHI2_ROW));
    printw(" PHI2[t=%2d]: ", cur_tick);

    for (int t = 0; t < 64; t++) {
        if (t == cur_tick) {
            attron(A_BOLD | A_REVERSE);
            addch('*');
            attroff(A_BOLD | A_REVERSE);
            attron(COLOR_PAIR(CP_PHI2_ROW));
            continue;
        }
        int phi      = (int)tmap[t];
        int prev_phi = (t > 0) ? (int)tmap[t - 1] : phi;
        char ch;
        if (phi < 0)       ch = '.';
        else if (phi == 1) ch = (prev_phi == 0) ? '/' : '-';
        else               ch = (prev_phi == 1) ? '\\' : '_';
        addch(ch);
    }

    clrtoeol();
    attron(COLOR_PAIR(CP_NORMAL));
}

/* Draw the filter status bar at row 2.
 * Shows active per-column filters, or a hint when none are set. */
static void draw_filter_bar(int cur_col,
                             char filter_str[][MAX_FILTER + 1])
{
    (void)cur_col;
    move(2, 0);
    attron(COLOR_PAIR(CP_FILTER_BAR) | A_BOLD);
    printw(" Filters:");
    attroff(A_BOLD);
    attron(COLOR_PAIR(CP_FILTER_BAR));
    int any = 0;
    for (int c = 0; c < NUM_COLUMNS; c++) {
        if (filter_str[c][0]) {
            printw(" [%s:\"%s\"]", COLUMNS[c].header, filter_str[c]);
            any = 1;
        }
    }
    if (!any)
        printw(" none  (f:set on cur col  F:clr  X:clr-all  Tab/BTab:jump)");
    else
        printw("  f:set  F:clr  X:clr-all  Tab/BTab:jump");
    clrtoeol();
    attron(COLOR_PAIR(CP_NORMAL));
    attroff(A_BOLD);
}

/* Draw the column header row (row 1).
 * Columns 0..LOCKED_COLS-1 are always shown on the left.  A coloured
 * lock-separator bar marks the boundary.  Scrollable columns start at
 * LOCKED_COLS+col_scroll and fill the remaining terminal width, with any
 * extra space distributed evenly across the visible columns. */
static void draw_header(int cur_col, int col_scroll,
                        char filter_str[][MAX_FILTER + 1])
{
    /* Precompute locked-section display width (content + inner seps + lock sep) */
    int lock_w = 1;  /* lock separator */
    for (int c = 0; c < LOCKED_COLS; c++)
        lock_w += COLUMNS[c].width + (c > 0 ? 1 : 0);

    /* First pass: count visible scrollable columns and their content width */
    int n_vis = 0, vis_w = 0;
    {
        int a = COLS - lock_w;
        for (int c = LOCKED_COLS + col_scroll; c < NUM_COLUMNS; c++) {
            int sep = n_vis > 0 ? 1 : 0;
            if (a < sep + COLUMNS[c].width) break;
            vis_w += sep + COLUMNS[c].width;
            a -= sep + COLUMNS[c].width;
            n_vis++;
        }
    }
    /* Distribute leftover space evenly (first xrem cols get one extra char) */
    int extra = (COLS - lock_w) - vis_w;
    int xpc   = n_vis > 0 ? extra / n_vis : 0;
    int xrem  = n_vis > 0 ? extra % n_vis : 0;

    move(1, 0);
    attron(COLOR_PAIR(CP_HEADER) | A_BOLD);

    /* Locked columns */
    for (int c = 0; c < LOCKED_COLS; c++) {
        if (c > 0) addch('|');
        if (c == cur_col) {
            attron(COLOR_PAIR(CP_CURCOL) | A_BOLD | A_UNDERLINE);
            printw("%*s", COLUMNS[c].width, COLUMNS[c].header);
            attroff(A_UNDERLINE);
            attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
        } else if (filter_str[c][0]) {
            attron(COLOR_PAIR(CP_FILTER_COL) | A_BOLD);
            printw("%*s", COLUMNS[c].width, COLUMNS[c].header);
            attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
        } else {
            printw("%*s", COLUMNS[c].width, COLUMNS[c].header);
        }
    }

    /* Lock separator */
    attron(COLOR_PAIR(CP_LOCK_SEP) | A_BOLD);
    addch('|');

    /* Scrollable columns with padding */
    for (int vis_i = 0, c = LOCKED_COLS + col_scroll; vis_i < n_vis; vis_i++, c++) {
        if (vis_i > 0) {
            attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
            addch('|');
        }
        if (c == cur_col) {
            attron(COLOR_PAIR(CP_CURCOL) | A_BOLD | A_UNDERLINE);
            printw("%*s", COLUMNS[c].width, COLUMNS[c].header);
            attroff(A_UNDERLINE);
        } else if (filter_str[c][0]) {
            attron(COLOR_PAIR(CP_FILTER_COL) | A_BOLD);
            printw("%*s", COLUMNS[c].width, COLUMNS[c].header);
        } else {
            attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
            printw("%*s", COLUMNS[c].width, COLUMNS[c].header);
        }
        int pad = xpc + (vis_i < xrem ? 1 : 0);
        if (pad > 0) {
            attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
            for (int p = 0; p < pad; p++) addch(' ');
        }
    }

    attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
    clrtoeol();
    attroff(A_BOLD);
}

/*
 * Draw one data row at screen line `screen_y`.
 *
 * Row colour rules (applied in priority order):
 *   cursor row + cursor col  → CP_CURCOL (yellow on blue, bold) — selected cell
 *   cursor row               → CP_CURSOR (blue background)
 *   CPU write  (r__w == 0)   → CP_WRITE  (yellow text)
 *   all others               → CP_NORMAL (terminal default)
 *
 * Additionally, if a search string is set and the cell in `cur_col`
 * matches it, that cell is highlighted with CP_MATCH (black-on-yellow)
 * regardless of the row colour.  On the cursor row, the match cell also
 * gets A_BOLD to remain visible against the blue background.
 */
static void draw_row(int screen_y, uint32_t idx,
                     const struct sysop_c64_bus_sample *s,
                     int is_cursor, int cur_col, int col_scroll,
                     const char *search_str,
                     char filter_str[][MAX_FILTER + 1])
{
    /* Determine whether this row passes all active column filters */
    int passes = 1;
    if (!is_cursor) {
        char fbuf[32];
        for (int c = 0; c < NUM_COLUMNS && passes; c++) {
            if (!filter_str[c][0]) continue;
            get_cell(fbuf, c, idx, s);
            if (!cell_matches(fbuf, filter_str[c])) passes = 0;
        }
    }

    int row_pair = is_cursor  ? CP_CURSOR
                 : (!passes   ? CP_DIMMED
                 : (s->r__w == 0 ? CP_WRITE : CP_NORMAL));
    chtype row_attr = (chtype)COLOR_PAIR(row_pair)
                    | (chtype)((!is_cursor && !passes) ? A_DIM : 0);

    char buf[32];

    /* Precompute layout (same as draw_header) */
    int lock_w = 1;
    for (int c = 0; c < LOCKED_COLS; c++)
        lock_w += COLUMNS[c].width + (c > 0 ? 1 : 0);
    int n_vis = 0, vis_w = 0;
    {
        int a = COLS - lock_w;
        for (int c = LOCKED_COLS + col_scroll; c < NUM_COLUMNS; c++) {
            int sep = n_vis > 0 ? 1 : 0;
            if (a < sep + COLUMNS[c].width) break;
            vis_w += sep + COLUMNS[c].width;
            a -= sep + COLUMNS[c].width;
            n_vis++;
        }
    }
    int extra = (COLS - lock_w) - vis_w;
    int xpc   = n_vis > 0 ? extra / n_vis : 0;
    int xrem  = n_vis > 0 ? extra % n_vis : 0;

    move(screen_y, 0);

    /* Locked columns */
    for (int c = 0; c < LOCKED_COLS; c++) {
        if (c > 0) {
            attron(row_attr);
            addch('|');
        }
        get_cell(buf, c, idx, s);
        int is_match       = (search_str[0] && c == cur_col
                              && cell_matches(buf, search_str));
        int is_cursor_cell = (is_cursor && c == cur_col && !is_match);
        if (is_match && is_cursor) attron(COLOR_PAIR(CP_MATCH) | A_BOLD);
        else if (is_match)         attron(COLOR_PAIR(CP_MATCH));
        else if (is_cursor_cell)   attron(COLOR_PAIR(CP_CURCOL) | A_BOLD);
        else if (c == cur_col)     attron(COLOR_PAIR(CP_CURCOL_SHADE));
        else                       attron(row_attr);
        addstr(buf);
    }

    /* Lock separator — always yellow-on-cyan regardless of row style */
    attron(COLOR_PAIR(CP_LOCK_SEP) | A_BOLD);
    addch('|');

    /* Scrollable columns with padding */
    for (int vis_i = 0, c = LOCKED_COLS + col_scroll; vis_i < n_vis; vis_i++, c++) {
        if (vis_i > 0) {
            attron(row_attr);
            addch('|');
        }
        get_cell(buf, c, idx, s);
        int is_match       = (search_str[0] && c == cur_col
                              && cell_matches(buf, search_str));
        int is_cursor_cell = (is_cursor && c == cur_col && !is_match);
        if (is_match && is_cursor) attron(COLOR_PAIR(CP_MATCH) | A_BOLD);
        else if (is_match)         attron(COLOR_PAIR(CP_MATCH));
        else if (is_cursor_cell)   attron(COLOR_PAIR(CP_CURCOL) | A_BOLD);
        else if (c == cur_col)     attron(COLOR_PAIR(CP_CURCOL_SHADE));
        else                       attron(row_attr);
        addstr(buf);
        /* Padding in row colour (not match colour) to fill terminal width */
        int pad = xpc + (vis_i < xrem ? 1 : 0);
        if (pad > 0) {
            attron(row_attr);
            for (int p = 0; p < pad; p++) addch(' ');
        }
    }

    attron(row_attr);
    clrtoeol();
}

/* Draw the status bar at the bottom of the screen (row LINES-1).
 * Shows position, frame number, VIC type, current column, search, and filter count. */
static void draw_status(int cur_row, int total, int cur_col,
                         const char *search_str,
                         char filter_str[][MAX_FILTER + 1],
                         const VicInfo *vic_info)
{
    int nfilters = 0;
    for (int c = 0; c < NUM_COLUMNS; c++)
        if (filter_str[c][0]) nfilters++;

    int fr = get_frame_number(vic_info, (uint32_t)cur_row);

    move(LINES - 1, 0);
    attron(A_REVERSE);
    printw(" %d/%d", cur_row + 1, total);
    if (vic_info->type != VIC_UNKNOWN) {
        if (fr >= 0)
            printw("  %s Fr %d/%d", vic_type_name(vic_info), fr + 1, vic_info->total_frames);
        else
            printw("  %s", vic_type_name(vic_info));
    }
    printw("  Col: %s [%d/%d]",
           COLUMNS[cur_col].header, cur_col + 1, NUM_COLUMNS);
    if (nfilters > 0) {
        printw("  Filters:%d", nfilters);
    }
    if (search_str[0])
        printw("  Search: \"%s\"  [n/N  Esc:clear]", search_str);
    else
        printw("  [/:search  f:filter]");
    clrtoeol();
    attroff(A_REVERSE);
}

/*
 * Interactive search-string input displayed on the status bar.
 * The user types a string; Enter confirms, Esc cancels (clears buf).
 * Backspace removes the last character.
 */
static void do_search_input(char *buf, int maxlen, int cur_col)
{
    int len = (int)strlen(buf);
    for (;;) {
        move(LINES - 1, 0);
        attron(A_REVERSE);
        printw(" Search %s: %s_", COLUMNS[cur_col].header, buf);
        clrtoeol();
        attroff(A_REVERSE);
        refresh();

        int ch = getch();
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            break;
        } else if (ch == 27) {          /* Esc — cancel */
            buf[0] = '\0';
            break;
        } else if ((ch == KEY_BACKSPACE || ch == 127 || ch == '\b') && len > 0) {
            buf[--len] = '\0';
        } else if (ch >= 32 && ch < 127 && len < maxlen) {
            buf[len++] = (char)ch;
            buf[len]   = '\0';
        }
    }
}

/*
 * do_filter_input — prompt for a per-column filter string on the status bar.
 * An empty string clears the filter for that column.  Esc also clears.
 */
static void do_filter_input(char *buf, int maxlen, int cur_col)
{
    int len = (int)strlen(buf);
    for (;;) {
        move(LINES - 1, 0);
        attron(A_REVERSE);
        printw(" Filter %s (empty=clear, Esc=cancel): %s_",
               COLUMNS[cur_col].header, buf);
        clrtoeol();
        attroff(A_REVERSE);
        refresh();

        int ch = getch();
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            break;
        } else if (ch == 27) {          /* Esc — cancel (leave buf unchanged) */
            break;
        } else if ((ch == KEY_BACKSPACE || ch == 127 || ch == '\b') && len > 0) {
            buf[--len] = '\0';
        } else if (ch >= 32 && ch < 127 && len < maxlen) {
            buf[len++] = (char)ch;
            buf[len]   = '\0';
        }
    }
}

/*
 * ensure_col_visible — adjust *col_scroll so that cur_col is within the
 * visible scrollable area.  Has no effect for locked columns (0..LOCKED_COLS-1).
 */
static void ensure_col_visible(int cur_col, int *col_scroll)
{
    if (cur_col < LOCKED_COLS) return;   /* locked — always visible */

    int scroll_idx = cur_col - LOCKED_COLS;

    /* Cursor must not be left of the scroll window */
    if (scroll_idx < *col_scroll)
        *col_scroll = scroll_idx;

    /* Compute locked-section display width (content + separators + lock bar) */
    int lock_w = 1;   /* lock separator */
    for (int c = 0; c < LOCKED_COLS; c++)
        lock_w += COLUMNS[c].width + (c > 0 ? 1 : 0);

    /* Advance col_scroll until cur_col fits within the visible scrollable area */
    for (;;) {
        int avail = COLS - lock_w;
        int last  = *col_scroll - 1;   /* index relative to LOCKED_COLS */
        for (int c = LOCKED_COLS + *col_scroll; c < NUM_COLUMNS; c++) {
            int sep = (c > LOCKED_COLS + *col_scroll) ? 1 : 0;
            if (avail < sep + COLUMNS[c].width) break;
            avail -= sep + COLUMNS[c].width;
            last = c - LOCKED_COLS;
        }
        if (last >= scroll_idx) break;  /* cur_col is now visible */
        (*col_scroll)++;
    }
}

/*
 * run_viewer — launch the interactive ncurses viewer.
 *
 * Screen layout (LINES rows total):
 *   Row 0          — title bar with key hints
 *   Row 1          — column header (current column underlined/highlighted)
 *   Row 2          — filter bar (active filters or hint)
 *   Row 3          — PHI2 waveform (current tick position visualised)
 *   Rows 4..LINES-2 — data rows  (LINES-5 visible at once)
 *   Row LINES-1    — status bar (position, column name, search/filter state)
 *
 * Samples are fetched on demand via source_get() — no sample buffer is
 * allocated.  Only the currently visible rows (typically < 50) are read
 * per frame, so even the full 12.5 M-sample buffer is fine.
 */
static void run_viewer(SampleSource *src)
{
    initscr();
    viewer_init_colors();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);        /* hide text cursor — we draw our own highlight */

    /* VIC info is populated lazily when the user presses Shift-V */
    VicInfo vic_info;
    memset(&vic_info, 0, sizeof(vic_info));
    vic_info.type = VIC_UNKNOWN;

    int cur_row    = 0;
    int top_row    = 0;
    int cur_col    = 0;
    int col_scroll = 0;   /* first visible scrollable column (offset from LOCKED_COLS) */
    char search_str[MAX_SEARCH + 1] = "";
    char filter_str[NUM_COLUMNS][MAX_FILTER + 1];
    memset(filter_str, 0, sizeof(filter_str));
    int snap_pending = 0;  /* set when filter changes; triggers one snap attempt */

    for (;;) {
        /* Number of data rows that fit between the PHI2 row and status bar */
        int visible = LINES - 5;
        if (visible < 1) visible = 1;

        /* Clamp cursor to valid range */
        if (cur_row < 0)                  cur_row = 0;
        if (cur_row >= (int)src->count)   cur_row = (int)src->count - 1;

        /* Check if any filter is active */
        int any_filter = 0;
        for (int c = 0; c < NUM_COLUMNS; c++)
            if (filter_str[c][0]) { any_filter = 1; break; }

        /* Pre-parse filter strings once per frame for fast per-sample checks */
        FilterFast filter_fast[NUM_COLUMNS];
        if (any_filter) compute_filter_fast(filter_str, filter_fast);

        /* When a filter is first applied (snap_pending), snap cur_row to the
         * nearest passing row.  snap_pending prevents a full re-scan on every
         * frame when no match exists. */
        if (any_filter && snap_pending) {
            snap_pending = 0;
            struct sysop_c64_bus_sample snap_s;
            source_get(src, (uint32_t)cur_row, &snap_s);
            if (!row_passes_filters((uint32_t)cur_row, &snap_s, filter_str)) {
                int snap_cancelled = 0;
                int found = filter_next(src, cur_row - 1, +1, filter_str, &snap_cancelled);
                if (found < 0 && !snap_cancelled)
                    found = filter_next(src, cur_row + 1, -1, filter_str, &snap_cancelled);
                if (found >= 0) { cur_row = found; top_row = found; }
            }
        }

        /* Scroll top_row to keep cur_row on screen.
         * Walk backwards from cur_row to top_row, counting matching rows.
         * If we reach 'visible' matches before hitting top_row, advance
         * top_row to the position of that visible-th match.  This is
         * O(visible * row_spacing) rather than O(cur_row - top_row).
         * Safety clamp: if the gap is larger than RENDER_ROW_LOOKAHEAD
         * (set by any large jump), reset top_row to cur_row so the scan
         * is O(1) and the jumped-to row appears at the top of the screen. */
        if (any_filter) {
            if (top_row > cur_row) top_row = cur_row;
            if (cur_row - top_row > RENDER_ROW_LOOKAHEAD) top_row = cur_row;
            struct sysop_c64_bus_sample s;
            int cnt = 0, new_top = top_row;
            for (int i = cur_row; i >= top_row; i--) {
                source_get(src, (uint32_t)i, &s);
                if (row_passes_filters_fast((uint32_t)i, &s, filter_str, filter_fast)) {
                    cnt++;
                    if (cnt == visible) { new_top = i; break; }
                }
            }
            if (cnt >= visible) top_row = new_top;
            if (top_row < 0) top_row = 0;
        } else {
            if (cur_row < top_row)               top_row = cur_row;
            if (cur_row >= top_row + visible)    top_row = cur_row - visible + 1;
            if (top_row < 0)                     top_row = 0;
        }

        /* ---- Render ---- */
        ensure_col_visible(cur_col, &col_scroll);
        draw_title(src->count, src->type == SRC_RAM, &vic_info);
        draw_header(cur_col, col_scroll, filter_str);
        draw_filter_bar(cur_col, filter_str);
        draw_phi2_waveform(3, (uint32_t)cur_row, src);

        if (any_filter) {
            /* Only render rows that pass all filters; blank the rest.
             * Once RENDER_ROW_LOOKAHEAD samples have been scanned without
             * finding the next matching row, stop scanning and blank the
             * remaining screen rows — avoids O(N) snprintf per frame when
             * the filter is sparse and most of the buffer has no matches. */
            int screen_i = 0;
            int si = top_row;
            int bail = 0;
            while (screen_i < visible) {
                int sy = 4 + screen_i;
                if (!bail) {
                    struct sysop_c64_bus_sample s;
                    int limit = si + RENDER_ROW_LOOKAHEAD;
                    if (limit > (int)src->count) limit = (int)src->count;
                    int found = 0;
                    while (si < limit) {
                        source_get(src, (uint32_t)si, &s);
                        if (row_passes_filters_fast((uint32_t)si, &s, filter_str, filter_fast)) {
                            draw_row(sy, (uint32_t)si, &s,
                                     si == cur_row, cur_col, col_scroll,
                                     search_str, filter_str);
                            screen_i++;
                            si++;
                            found = 1;
                            break;
                        }
                        si++;
                    }
                    if (!found) bail = 1;
                }
                if (bail) {
                    move(sy, 0);
                    attron(COLOR_PAIR(CP_NORMAL));
                    clrtoeol();
                    screen_i++;
                }
            }
        } else {
            for (int i = 0; i < visible; i++) {
                int ri = top_row + i;
                int sy = 4 + i;
                if (ri < (int)src->count) {
                    struct sysop_c64_bus_sample s;
                    source_get(src, (uint32_t)ri, &s);
                    draw_row(sy, (uint32_t)ri, &s,
                             ri == cur_row, cur_col, col_scroll, search_str,
                             filter_str);
                } else {
                    move(sy, 0);
                    attron(COLOR_PAIR(CP_NORMAL));
                    clrtoeol();
                }
            }
        }

        draw_status(cur_row, (int)src->count, cur_col, search_str, filter_str, &vic_info);
        refresh();

        /* ---- Input ---- */
        int ch = getch();
        int cancelled = 0;  /* set by scan functions if user pressed Esc */
        switch (ch) {

        /* --- Retrigger sampler (RAM source only) --- */
        case 'T':
            if (src->type == SRC_RAM) {
                /* Phase 1: arm the sampler */
                move(LINES - 1, 0);
                attron(A_REVERSE | A_BOLD);
                printw(" Arming sampler... ");
                clrtoeol();
                attroff(A_REVERSE | A_BOLD);
                refresh();
                sysop_sampler_start();
                /* Brief pause so the hardware has time to transition to busy
                 * before we poll for completion. */
                usleep(50000);   /* 50 ms */
                /* Phase 2: wait for the capture to complete */
                move(LINES - 1, 0);
                attron(A_REVERSE | A_BOLD);
                printw(" Waiting for capture to complete... ");
                clrtoeol();
                attroff(A_REVERSE | A_BOLD);
                refresh();
                sysop_sampler_wait_not_busy();
                /* Reset VIC info — user can re-run detection with Shift-V */
                memset(&vic_info, 0, sizeof(vic_info));
                vic_info.type = VIC_UNKNOWN;
                /* Reload: reset position to start of the new capture */
                cur_row    = 0;
                top_row    = 0;
                col_scroll = 0;
                snap_pending = any_filter ? 1 : 0;
                clearok(stdscr, TRUE);   /* force full redraw */
            }
            break;

        /* --- Quit --- */
        case 'q': case 'Q':
            goto done;
        case 27:    /* Esc: clear search if active, otherwise quit */
            if (search_str[0]) search_str[0] = '\0';
            else goto done;
            break;

        /* --- Row navigation (filter-aware when a filter is active) --- */
        case KEY_UP: case 'k':
            if (any_filter) {
                int found = filter_next(src, cur_row, -1, filter_str, &cancelled);
                if (found >= 0) cur_row = found;
            } else {
                cur_row--;
            }
            break;
        case KEY_DOWN: case 'j':
            if (any_filter) {
                int found = filter_next(src, cur_row, +1, filter_str, &cancelled);
                if (found >= 0) cur_row = found;
            } else {
                cur_row++;
            }
            break;
        case KEY_PPAGE: case 'b':
            if (any_filter) {
                int r = cur_row;
                for (int i = 0; i < visible && !cancelled; i++) {
                    int prev = filter_next(src, r, -1, filter_str, &cancelled);
                    if (prev < 0) break;
                    r = prev;
                }
                cur_row = r;
                top_row = r;    /* place cursor at top of new page */
            } else {
                cur_row -= visible;
            }
            break;
        case KEY_NPAGE: case ' ':
            if (any_filter) {
                int r = cur_row;
                for (int i = 0; i < visible && !cancelled; i++) {
                    int next = filter_next(src, r, +1, filter_str, &cancelled);
                    if (next < 0) break;
                    r = next;
                }
                if (!cancelled) { cur_row = r; top_row = r; }
            } else {
                cur_row += visible;
            }
            break;
        case KEY_HOME: case 'g':
            if (any_filter) {
                int found = filter_next(src, -1, +1, filter_str, &cancelled);
                if (found >= 0) { cur_row = found; top_row = found; }
            } else {
                cur_row = 0;
            }
            break;
        case KEY_END: case 'G':
            if (any_filter) {
                int found = filter_next(src, (int)src->count, -1, filter_str, &cancelled);
                if (!cancelled && found >= 0) { cur_row = found; top_row = found; }
            } else {
                cur_row = (int)src->count - 1;
            }
            break;

        /* --- Column navigation --- */
        case KEY_LEFT:  case 'h':
            if (cur_col > 0) cur_col--;
            break;
        case KEY_RIGHT: case 'l':
            if (cur_col < NUM_COLUMNS - 1) cur_col++;
            break;

        /* --- Search --- */
        case '/': {
            /* Prompt for a search string, then jump to first match
             * at or after the current row. */
            do_search_input(search_str, MAX_SEARCH, cur_col);
            if (search_str[0]) {
                /* search_next starts at from+dir, so from=cur_row-1
                 * makes the first candidate cur_row itself. */
                int found = search_next(src, cur_col,
                                        cur_row - 1, 1, search_str, &cancelled);
                if (found >= 0) cur_row = found;
                else if (!cancelled) beep();
            }
            break;
        }
        case 'n':   /* Next match after cursor */
            if (search_str[0]) {
                int found = search_next(src, cur_col,
                                        cur_row, 1, search_str, &cancelled);
                if (found >= 0) cur_row = found;
                else if (!cancelled) beep();
            }
            break;
        case 'N':   /* Previous match before cursor */
            if (search_str[0]) {
                int found = search_next(src, cur_col,
                                        cur_row, -1, search_str, &cancelled);
                if (found >= 0) cur_row = found;
                else if (!cancelled) beep();
            }
            break;

        /* --- Filter set/clear --- */
        case 'f': {   /* Set/update filter on current column */
            do_filter_input(filter_str[cur_col], MAX_FILTER, cur_col);
            top_row = cur_row;  /* reset window after filter change */
            snap_pending = 1;   /* snap cursor to first match on next frame */
            break;
        }
        case 'F':     /* Clear filter on current column */
            filter_str[cur_col][0] = '\0';
            top_row = cur_row;
            break;
        case 'X':     /* Clear all filters */
            memset(filter_str, 0, sizeof(filter_str));
            top_row = cur_row;
            break;

        /* --- VIC type detection (on demand) --- */
        case 'V':
            move(LINES - 1, 0);
            attron(A_REVERSE | A_BOLD);
            printw(" Detecting VIC type... ");
            clrtoeol();
            attroff(A_REVERSE | A_BOLD);
            refresh();
            detect_vic_info(src, &vic_info);
            break;

        /* --- Filter navigation (also works when no filter set) --- */
        case '\t': {  /* Tab: jump to next row passing all filters */
            int found = filter_next(src, cur_row, 1, filter_str, &cancelled);
            if (found >= 0) cur_row = found;
            else if (!cancelled) beep();
            break;
        }
        case KEY_BTAB: {  /* Shift+Tab: jump to prev row passing all filters */
            int found = filter_next(src, cur_row, -1, filter_str, &cancelled);
            if (found >= 0) cur_row = found;
            else if (!cancelled) beep();
            break;
        }
        }
    }
done:
    endwin();
}

/* ================================================================== */
/* Data loading (viewer mode) and streaming CSV (default mode)        */
/* ================================================================== */

/*
 * dump_csv_from_ram — streaming CSV output from FPGA RAM (no malloc).
 * Suitable for large sample counts that would be impractical to load.
 */
static int dump_csv_from_ram(uint32_t count, int trigger, int prefix)
{
    if (count > SYSOP64_SAMPLER_MAX_SAMPLES) {
        fprintf(stderr, "Requested %u samples exceeds hardware maximum %u\n",
                count, SYSOP64_SAMPLER_MAX_SAMPLES);
        return 1;
    }
    sysop_init();
    if (trigger) {
        sysop_sampler_start();
        sysop_sampler_wait_not_busy();
    }
    print_csv_header(prefix);
    struct sysop_c64_bus_sample s;
    for (uint32_t i = 0; i < count; i++) {
        sysop_sampler_get_sample(i, &s);
        print_csv_row(i, &s, prefix);
    }
    sysop_uninit();
    return 0;
}

/*
 * dump_csv_from_file — streaming CSV decode from a binary capture file.
 * No hardware connection required.
 */
static int dump_csv_from_file(const char *filename, uint32_t count, int prefix)
{
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "Cannot open '%s': ", filename);
        perror(NULL);
        return 1;
    }
    print_csv_header(prefix);
    struct sysop_c64_bus_sample s;
    uint64_t raw;
    for (uint32_t i = 0; i < count; i++) {
        if (fread(&raw, sizeof(raw), 1, fp) < 1) {
            fprintf(stderr, "End of file at sample %u\n", i);
            break;
        }
        decode_sample_raw(raw, &s);
        print_csv_row(i, &s, prefix);
    }
    fclose(fp);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "Usage:\n"
                "  c64busview [ram] [<count>] [--trigger-sampler] [--viewer]\n"
                "  c64busview <filename> [<count>] [--viewer]\n"
                "\n"
                "  Source defaults to 'ram' when omitted.\n"
                "  <count> defaults to SYSOP64_SAMPLER_MAX_SAMPLES (%u) when omitted.\n"
                "\n"
                "  ram                 Read current FPGA sample buffer (no retrigger).\n"
                "  --trigger-sampler   Arm sampler and wait for capture to complete.\n"
                "  --viewer            Launch interactive terminal viewer instead of CSV.\n"
                "  --no-prefix         CSV output: plain numbers only (no L/CYC/IRQ= labels).\n"
                "  <filename>          Decode a previously saved binary capture file.\n"
                "\n"
                "CSV output examples:\n"
                "  c64busview ram 10000 > capture.csv\n"
                "  c64busview --trigger-sampler > capture.csv\n"
                "  c64busview capture.bin 10000 > decoded.csv\n"
                "\n"
                "Viewer examples:\n"
                "  c64busview --viewer --trigger-sampler\n"
                "  c64busview ram 50000 --viewer\n"
                "  c64busview capture.bin 10000 --viewer\n",
                SYSOP64_SAMPLER_MAX_SAMPLES);
        return 1;
    }

    /* If argv[1] is a flag, source defaults to "ram" and we start scanning
     * flags immediately; otherwise argv[1] is the source (ram or filename). */
    const char *source;
    int flags_start;
    if (argv[1][0] == '-') {
        source      = "ram";
        flags_start = 1;
    } else {
        source      = argv[1];
        flags_start = 2;
    }

    /* Count is optional.  If the next positional arg is absent or starts
     * with '--' it is a flag; default to the full hardware buffer size. */
    uint32_t count = SYSOP64_SAMPLER_MAX_SAMPLES;
    if (flags_start < argc && argv[flags_start][0] != '-') {
        count = (uint32_t)strtoul(argv[flags_start], NULL, 10);
        if (count == 0) {
            fprintf(stderr, "Sample count must be greater than 0\n");
            return 1;
        }
        flags_start++;
    }

    /* Scan optional flags */
    int trigger = 0, viewer = 0, prefix = 1;
    for (int i = flags_start; i < argc; i++) {
        if      (strcmp(argv[i], "--trigger-sampler") == 0) trigger = 1;
        else if (strcmp(argv[i], "--viewer")          == 0) viewer  = 1;
        else if (strcmp(argv[i], "--no-prefix")       == 0) prefix  = 0;
        else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    int is_ram = (strcmp(source, "ram") == 0);

    if (viewer) {
        SampleSource src;
        src.fp = NULL;
        if (is_ram) {
            if (count > SYSOP64_SAMPLER_MAX_SAMPLES) {
                fprintf(stderr,
                        "Requested %u samples exceeds hardware maximum %u\n",
                        count, SYSOP64_SAMPLER_MAX_SAMPLES);
                return 1;
            }
            src.type  = SRC_RAM;
            src.count = count;
            sysop_init();
            if (trigger) {
                sysop_sampler_start();
                sysop_sampler_wait_not_busy();
            }
            run_viewer(&src);
            sysop_uninit();
        } else {
            FILE *fp = fopen(source, "rb");
            if (!fp) {
                fprintf(stderr, "Cannot open '%s': ", source);
                perror(NULL);
                return 1;
            }
            /* Derive count from file size — every sample is 8 bytes. */
            fseek(fp, 0, SEEK_END);
            long fsz = ftell(fp);
            rewind(fp);
            uint32_t file_count = (fsz > 0)
                ? (uint32_t)((unsigned long)fsz / sizeof(uint64_t)) : 0;
            if (count > file_count) {
                /* Warn only when the user explicitly asked for more */
                if (count != SYSOP64_SAMPLER_MAX_SAMPLES)
                    fprintf(stderr,
                            "Note: file contains %u samples (requested %u)\n",
                            file_count, count);
                count = file_count;
            }
            src.type  = SRC_FILE;
            src.fp    = fp;
            src.count = count;
            run_viewer(&src);
            fclose(fp);
        }
    } else {
        /* CSV mode: stream directly, no allocation */
        if (is_ram)
            return dump_csv_from_ram(count, trigger, prefix);
        else
            return dump_csv_from_file(source, count, prefix);
    }

    return 0;
}

