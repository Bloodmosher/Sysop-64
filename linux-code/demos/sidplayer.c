/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 *
 * DESCRIPTION:
 *     SID file player demo.
 *     Loads a .sid file, prints title/author, and plays it on the real C64
 *     by writing SID register values each VBlank via the sysop bridge.
 *
 * USAGE:
 *     sidplayer <file.sid> [subtune]
 *
 *     subtune: 0-based index, defaults to 0.
 *
 * EXAMPLE:
 *     sidplayer /path/to/tune.sid
 *     sidplayer /path/to/tune.sid 2
 */

#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sysop64.h"
#include "sysop_sid.h"

/* ------------------------------------------------------------------ */
/* Globals                                                              */
/* ------------------------------------------------------------------ */

static volatile int g_running = 1;
static SidPlayerHandle *g_player = NULL;

static void cleanup(void)
{
    int i;
    if (g_player) {
        sid_player_stop(g_player);
        sid_player_destroy(g_player);
        g_player = NULL;
    }
    /* Silence all SID voices before releasing DMA. */
    for (i = 0; i < 25; i++)
        sysop_poke(0xD400 + i, 0);
    sysop_server_dma_unlock();
    sysop_server_disconnect();
    sysop_uninit();
}

static void handle_sigint(int sig)
{
    g_running = 0;
    cleanup();
    exit(128 + sig);
}

/* ------------------------------------------------------------------ */
/* Digi write scheduler                                                 */
/* ------------------------------------------------------------------ */

/*
 * VIC-II bad-line constants (assume default YSCROLL = 3).
 * On every line in the display area (48-247) where (line % 8) == YSCROLL,
 * the VIC steals the bus from the CPU for cycles 15-54 (~40 cycles).
 * The first cycle where the CPU can access the bus again is cycle 55.
 */
#define YSCROLL_DEFAULT   3
#define BAD_LINE_FIRST   48
#define BAD_LINE_LAST   247
#define BAD_CYCLE_FIRST  15
#define BAD_CYCLE_LAST   54
#define BAD_CYCLE_SAFE   55   /* first safe cycle after the stolen window */

/* Frame-absolute index: (line * cpl) + (cycle - 1), cycle is 1-based. */
static int vic_abs(int line, int cycle, int cpl)
{
    return line * cpl + (cycle - 1);
}

/* Return 1 if (line, cycle) is inside a bad-line stolen window. */
static int is_bad_cycle(int line, int cycle)
{
    if (line < BAD_LINE_FIRST || line > BAD_LINE_LAST) return 0;
    if ((line % 8) != YSCROLL_DEFAULT) return 0;
    return cycle >= BAD_CYCLE_FIRST && cycle <= BAD_CYCLE_LAST;
}

/*
 * If abs falls in a bad-line stolen window, advance it to BAD_CYCLE_SAFE
 * of that line (first cycle where the CPU can access the bus).
 */
static int skip_bad_window(int abs, int cpl)
{
    int line  = abs / cpl;
    int cycle = abs % cpl + 1; /* back to 1-based */
    if (is_bad_cycle(line, cycle))
        abs = vic_abs(line, BAD_CYCLE_SAFE, cpl);
    return abs;
}

/*
 * Deliver one frame of raster-timed digi writes efficiently.
 *
 * Tracks the current VIC bus position as a frame-absolute cycle index.
 * sysop_wait_vic2() is only called when the target is strictly ahead of the
 * current position — consecutive writes at the same cycle (e.g. the
 * $D020 DEC and $D418 nibble produced for every NMI) or writes that
 * land within 1-2 cycles of each other skip the wait entirely.
 *
 * Writes targeting a bad-line stolen window (cycles 15-54 of every 8th
 * display line) are pushed to cycle BAD_CYCLE_SAFE (55) before the wait.
 *
 *
 * cpl        : cycles per line  (PAL 6569 = 63, NTSC 6567R8 = 65)
 * total_lines: lines per frame  (PAL = 312, NTSC 6567R8 = 263)
 */
static void deliver_digis(const SidDigiWrite *digis, int num_digis,
                          int cpl, int total_lines)
{
    int cur_abs = -1;   /* -1 = unknown; always wait on first entry */
    int i;

    for (i = 0; i < num_digis; i++) {
        int tgt_line  = (int)digis[i].raster_line;
        int tgt_cycle = (int)digis[i].cycle;

        /* Clamp to valid frame range. */
        if (tgt_line  >= total_lines) tgt_line  = total_lines - 1;
        if (tgt_cycle < 1)            tgt_cycle = 1;
        if (tgt_cycle > cpl)          tgt_cycle = cpl;

        /* Push out of bad-line stolen window before converting to abs. */
        if (is_bad_cycle(tgt_line, tgt_cycle))
            tgt_cycle = BAD_CYCLE_SAFE;

        int tgt_abs = vic_abs(tgt_line, tgt_cycle, cpl);

        if (cur_abs < 0 || tgt_abs > cur_abs) {
            /* Hardware hasn't reached the target yet — wait for it. */
            sysop_wait_vic2((uint16_t)tgt_line, (uint8_t)tgt_cycle);
            cur_abs = tgt_abs;
        }
        /*
         * else: target is at or before current position.
         * The write lands at cur_abs (one or two cycles late at most),
         * which is acceptable given emulator cycle-approximation error.
         */

        if (digis[i].addr == 0xd020) {
            sysop_poke(0xd020, digis[i].val == 0 ? 1 : 0);
        }
        else 
            sysop_poke(digis[i].addr, digis[i].val);

        /* Advance by one cycle; skip into the bad-line safe zone if needed. */
        cur_abs = skip_bad_window(cur_abs + 1, cpl);
    }
}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static void print_time(float elapsed, float duration)
{
    int em = (int)(elapsed / 60);
    int es = (int)elapsed % 60;
    if (duration > 0) {
        int dm = (int)(duration / 60);
        int ds = (int)duration % 60;
        printf("\r  %02d:%02d / %02d:%02d  ", em, es, dm, ds);
    } else {
        printf("\r  %02d:%02d        ", em, es);
    }
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    const char *path;
    int subtune = 0;
    SidPlayerHandle *player;
    char title[33];
    char author[33];
    SidPoke      pokes[64];
    SidDigiWrite digis[1024];
    int num_pokes, num_digis;
    int cpl, total_lines;
    float elapsed, duration;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file.sid> [subtune]\n", argv[0]);
        return 1;
    }

    path    = argv[1];
    subtune = (argc >= 3) ? atoi(argv[2]) : 0;

    signal(SIGINT, handle_sigint);

    /* ---- Connect to sysop server and acquire DMA ---- */
    sysop_init();
    if (sysop_server_connect() != 0) {
        fprintf(stderr, "sidplayer: failed to connect to sysop server\n");
        sysop_uninit();
        return 1;
    }
    sysop_server_dma_lock();

    /* ---- Create player (PAL by default; SID header clock hint may override) ---- */
    player = sid_player_create(/*is_pal=*/1);
    g_player = player;
    if (!player) {
        fprintf(stderr, "sidplayer: out of memory\n");
        sysop_server_dma_unlock();
        sysop_server_disconnect();
        sysop_uninit();
        return 1;
    }

    /* ---- Load ---- */
    printf("Loading: %s  (subtune %d)\n", path, subtune);
    if (!sid_player_load(player, path, subtune)) {
        fprintf(stderr, "sidplayer: failed to load '%s'\n", path);
        cleanup();
        return 1;
    }

    /* Derive VIC timing constants from the actual hardware chip type.
     *   6567R56A (NTSC old) : 64 cycles/line, 262 lines
     *   6567R8   (NTSC new) : 65 cycles/line, 263 lines
     *   6569     (PAL)      : 63 cycles/line, 312 lines
     *   6572     (DREAN)    : 65 cycles/line, 312 lines
     * These are used by deliver_digis() for position tracking and bad-line
     * detection, independent of whether the SID tune is flagged PAL or NTSC. */
    {
        uint8_t vic = sysop_get_vic_info();
        switch (vic) {
        case VIC_CHIP_6567R56A:
            cpl = 64; total_lines = 262;
            printf("VIC: 6567R56A (NTSC old, 64 cyc/line, 262 lines)\n");
            break;
        case VIC_CHIP_6567R8:
            cpl = 65; total_lines = 263;
            printf("VIC: 6567R8 (NTSC, 65 cyc/line, 263 lines)\n");
            break;
        case VIC_CHIP_6572RO_DREAN:
            cpl = 65; total_lines = 312;
            printf("VIC: 6572 DREAN (PAL-N, 65 cyc/line, 312 lines)\n");
            break;
        default: /* VIC_CHIP_6569 */
            cpl = 63; total_lines = 312;
            printf("VIC: 6569 (PAL, 63 cyc/line, 312 lines)\n");
            break;
        }
    }

    sid_player_get_title(player, title, sizeof(title));
    sid_player_get_author(player, author, sizeof(author));
    printf("Title:  %s\n", title);
    printf("Author: %s\n", author);
    printf("Press Ctrl+C to stop.\n");

    duration = sid_player_get_duration_seconds(player);

    /* ---- Playback loop ---- */
    uint32_t frame_tag = 0;

    while (g_running && sid_player_is_playing(player)) {
        /* Wait for the hardware to echo the tag we wrote at the start of the
         * previous frame.  When it arrives the DMA queue is one frame deep
         * and it is safe to queue the next frame's writes. */
        if (frame_tag != 0) {
            uint32_t tag;
            do {
                tag = sysop_dma_tag_data();
                usleep(50);
            } while (tag != frame_tag);
        }

        /* Tag the beginning of this frame's DMA batch. */
        frame_tag++;
        sysop_dma_write_tag(frame_tag);

        
        sid_player_play_frame_ex(player,
                                 pokes, 64,   &num_pokes,
                                 digis, 1024, &num_digis);

        /* Apply regular SID register writes. */
        int i;
        for (i = 0; i < num_pokes; i++)
            sysop_poke(pokes[i].addr, pokes[i].val);

        deliver_digis(digis, num_digis, cpl, total_lines);

        if (num_digis == 0) {
            sysop_wait_vic2(250, 1);
        }

        elapsed = sid_player_get_elapsed_seconds(player);
        print_time(elapsed, duration);
    }

    printf("\n");

    /* ---- Mute and clean up ---- */
    cleanup();
    return 0;
}
