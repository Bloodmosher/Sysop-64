/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * digiplay — raw digi sample playback through the SID volume register.
 *
 * Background — how C64 digi playback works
 * -----------------------------------------
 * The SID chip's master volume register ($D418) has a 4-bit DAC that
 * leaks into the audio output.  By rapidly writing different 4-bit
 * values to that register, software can synthesise arbitrary waveforms
 * ("digi" playback).  No PCM DMA exists; every sample write is a CPU
 * store to $D418, which is what this program does from the host side
 * via the Sysop-64 DMA bridge.
 *
 * File format
 * -----------
 * The raw file is expected to contain unsigned 8-bit PCM samples (one
 * byte per sample, values 0–255).  Only the upper nibble of each byte
 * is written to $D418, giving 4-bit resolution that matches the SID's
 * DAC depth.  Standard C64 digi players (e.g. GoDot, Rockoon) use the
 * same convention.
 *
 * Sample rate
 * -----------
 * The inter-sample delay is controlled by the optional second command-
 * line argument (microseconds).  The default of 1 µs yields roughly
 * the maximum throughput the bridge can sustain.  For reference:
 *
 *   ~8000  µs/sample → ~125 Hz  (very low quality)
 *   ~  62  µs/sample → ~16 kHz  (typical C64 digi quality)
 *   ~   1  µs/sample → as fast as possible (stress test / high rate)
 *
 * Usage
 * -----
 *   digiplay <path-to-raw-file> [delay-us]
 *
 *   e.g.  digiplay blood_mosher.raw 62
 *
 * The program plays the file once, then exits cleanly.
 * Press Ctrl+C at any time to stop and restore the C64.
 */

#include <error.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <signal.h>
#include "sysop64.h"

/* --------------------------------------------------------------------------
 * Global state needed by the signal handler
 * -------------------------------------------------------------------------- */

/* Pointer to the sample buffer so the SIGINT handler can free it. */
static unsigned char *g_buffer = NULL;

/* --------------------------------------------------------------------------
 * Signal handler — clean shutdown on Ctrl+C
 * -------------------------------------------------------------------------- */

/*
 * sigint_handler — called when the user presses Ctrl+C.
 *
 * Frees the sample buffer, releases the DMA lock, and disconnects from
 * the Sysop-64 bridge so the C64 is left in a clean state.  Silencing
 * $D418 (writing 0) before unlocking avoids a stuck audio click.
 */
static void sigint_handler(int sig)
{
    /* Silence the SID DAC before we exit so there is no stuck audio level. */
    sysop_poke(0xd418, 0);

    free(g_buffer);
    g_buffer = NULL;

    sysop_dma_disable();
    sysop_server_dma_unlock();
    sysop_server_disconnect();
    sysop_uninit();

    exit(sig);
}

/* --------------------------------------------------------------------------
 * Entry point
 * -------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: %s <raw-file> [delay-us]\n", argv[0]);
        printf("  raw-file  : unsigned 8-bit PCM, upper nibble written to $D418\n");
        printf("  delay-us  : microseconds between samples (default: 1)\n");
        return -1;
    }

    /*
     * Optional second argument sets the inter-sample delay in microseconds.
     * Smaller values produce higher sample rates at the cost of more DMA
     * traffic.  The default of 1 µs is essentially "as fast as possible".
     */
    int delay_us = 1;
    if (argc > 2)
        delay_us = (int)strtoll(argv[2], NULL, 10);

    signal(SIGINT, sigint_handler);

    /* ------------------------------------------------------------------
     * Connect to the Sysop-64 bridge and acquire exclusive DMA access.
     * sysop_server_dma_lock() must be called before any sysop_poke() operations so
     * the host can drive the C64 address/data bus without bus conflicts.
     * ------------------------------------------------------------------ */
    sysop_init();

    int res = sysop_server_connect();
    if (res != 0) {
        fprintf(stderr, "sysop_server_connect() failed\n");
        return -1;
    }

    sysop_server_dma_lock();
    sysop_dma_enable();

    /* ------------------------------------------------------------------
     * Load the raw sample file into memory.
     * ------------------------------------------------------------------
     * We read the whole file up front so the playback loop is as tight
     * as possible — no I/O latency during sample streaming.
     * ------------------------------------------------------------------ */
    const char *filename = argv[1];

    printf("Loading %s ...\n", filename);

    FILE *file = fopen(filename, "rb");
    if (file == NULL) {
        fprintf(stderr, "Error: could not open '%s'\n", filename);
        goto do_exit;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    g_buffer = (unsigned char *)malloc(file_size);
    if (g_buffer == NULL) {
        fprintf(stderr, "Error: malloc(%ld) failed\n", file_size);
        fclose(file);
        goto do_exit;
    }

    fread(g_buffer, sizeof(unsigned char), file_size, file);
    fclose(file);

    printf("Playing %ld bytes, delay %d µs/sample ...\n", file_size, delay_us);

    /* ------------------------------------------------------------------
     * Playback loop
     * ------------------------------------------------------------------
     * Write the upper nibble of each sample byte to $D418 (the SID
     * master-volume / DAC register).  The lower nibble is discarded
     * because the SID's DAC only has 4-bit resolution.
     *
     * $D418 also holds the filter-enable bits for voices 1–3 in bits
     * 0–2 and the filter-to-output routing in bit 3.  Those bits are
     * left as zero here, which disables the filter and routes all three
     * voices to the output unmuted — suitable for a standalone digi
     * player where SID tone voices are not in use.
     * ------------------------------------------------------------------ */
    sysop_poke(0xd020, 0);   /* black border while playing */

    for (long index = 0; index < file_size; index++) {
        /*
         * Mask to 4 bits: the SID volume DAC is 4-bit wide.  The upper
         * nibble of the raw sample byte maps directly to the 0–15 range.
         */
        uint8_t nibble = (g_buffer[index] >> 4) & 0x0F;
        sysop_poke(0xd418, nibble);

        if (delay_us > 0)
            usleep(delay_us);
    }

    printf("Done.\n");

do_exit:
    /* Silence the DAC so there is no stuck audio level after playback. */
    sysop_poke(0xd418, 0);

    free(g_buffer);
    g_buffer = NULL;

    sysop_dma_disable();
    sysop_server_dma_unlock();
    sysop_server_disconnect();
    sysop_uninit();

    return 0;
}
