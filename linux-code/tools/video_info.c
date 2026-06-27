/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 *
 * video_info — display FPGA core version, VIC chip model, and live
 * line-timing diagnostics.
 *
 * Prints the FPGA core version and detected VIC model once at startup,
 * then enters a continuous monitoring loop that samples CPU frequency and
 * line-timing registers every 2 seconds, printing them on a single
 * updating line.  Press Ctrl-C to exit.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <stdbool.h>
#include "sysop64.h"

/*
 * bcd_to_binary — convert a packed BCD value to a plain binary integer.
 *
 * Each group of 4 bits holds one decimal digit (0–9).  The function works
 * from the least-significant digit upward, accumulating the result.
 */
uint32_t bcd_to_binary(uint32_t bcd)
{
    uint32_t result = 0;
    uint32_t multiplier = 1;

    while (bcd > 0)
    {
        uint8_t digit = bcd & 0xF; /* extract the lowest BCD digit (4 bits) */
        result += digit * multiplier;
        multiplier *= 10;
        bcd >>= 4; /* advance to the next digit */
    }

    return result;
}

/*
 * bcd_extract_range — extract a sub-range of BCD digits and return the
 * corresponding binary integer.
 *
 * Digits are numbered 0 (least significant nibble) through 7.
 * first_digit..last_digit is the inclusive range to extract.
 * Returns 0 on an invalid range and 0xFFFFFFFF if any digit exceeds 9.
 */
uint32_t bcd_extract_range(uint32_t bcd, int last_digit, int first_digit)
{
    if (first_digit < 0 || last_digit > 7 || first_digit > last_digit)
        return 0;

    uint32_t result = 0;
    uint32_t multiplier = 1;

    for (int i = first_digit; i <= last_digit; i++)
    {
        uint8_t digit = (bcd >> (i * 4)) & 0xF;

        if (digit > 9)
            return 0xFFFFFFFF; /* invalid BCD digit */

        result += digit * multiplier;
        multiplier *= 10;
    }

    return result;
}

int main(int argc, char **argv)
{
    sysop_init();

    /* --- FPGA core version --- */
    uint32_t sysop_version = sysop_get_version_info();
    printf("FPGA Core: %d.%d.%d.%d\n",
        (sysop_version & 0xFF000000) >> 24,
        (sysop_version & 0x00FF0000) >> 16,
        (sysop_version & 0x0000FF00) >>  8,
        (sysop_version & 0x000000FF));

    /* --- VIC chip model ---
     * Bit 7 of vic_info indicates whether the FPGA has finished auto-detecting
     * the VIC revision.  Bits 2:0 identify the specific chip variant. */
    uint8_t vic_info = sysop_get_vic_info();
    if (!(vic_info & 0x80))
    {
        printf("FPGA has not yet determined VIC model.\n");
    }
    else
    {
        printf("VIC Model: ");
        switch (vic_info & 0x7)
        {
            case VIC_CHIP_6567R56A:     printf("NTSC OLD"); break;
            case VIC_CHIP_6567R8:       printf("NTSC NEW"); break;
            case VIC_CHIP_6572RO_DREAN: printf("DREAN");    break;
            case VIC_CHIP_6569:         printf("PAL");      break;
            default:                    printf("Unknown");  break;
        }
        printf("\n");
    }

    /* --- Continuous line-timing monitor ---
     *
     * sysop_get_line_timing() returns a packed BCD word:
     *   digits 7:5 — current HDMI output line
     *   digits 4:2 — current VIC raster line
     *   digits 1:0 — sync reset counter (increments on each VIC sync)
     *
     * We track when the HDMI line value changes and measure how long it
     * stayed on the previous value, giving a rough frame/change rate. */
    uint32_t samples = 0;
    uint32_t prev_hdmi_line = 0xFFFFFFFF; /* sentinel: no previous value yet */
    struct timespec start_time, end_time;
    uint64_t change_rate_ms = 0;

    printf("Continuous monitoring, hit CTRL-C to exit...\n");
    while (1)
    {
        uint32_t cpu_freq_bcd = sysop_get_cpu_freq();
        uint32_t cpu_freq     = bcd_to_binary(cpu_freq_bcd);
        uint32_t line_timing  = sysop_get_line_timing();

        uint32_t hdmi_line = bcd_extract_range(line_timing, 7, 5);
        uint32_t vic_line  = bcd_extract_range(line_timing, 4, 2);
        uint32_t resets    = line_timing & 0xFF;

        /* Detect a change in HDMI line and measure how long since the last. */
        bool changed = false;
        if (hdmi_line != prev_hdmi_line)
        {
            if (prev_hdmi_line != 0xFFFFFFFF) /* skip timing on the very first sample */
            {
                changed = true;
                clock_gettime(CLOCK_MONOTONIC, &end_time);
                change_rate_ms =
                    ((end_time.tv_sec  - start_time.tv_sec)  * 1000ULL) +
                    ((end_time.tv_nsec - start_time.tv_nsec) / 1000000ULL);
            }
            prev_hdmi_line = hdmi_line;
            clock_gettime(CLOCK_MONOTONIC, &start_time);
        }

        if (changed)
            printf("\n");

        /* Overwrite the current line in-place using CR + erase-line escape. */
        printf("\r\033[2KSample: %u  CPU: %u Hz  VIC Line: %u  Prev: %u  Resets: %u  Change: %" PRIu64 " ms",
               samples++, cpu_freq, hdmi_line, vic_line, resets, change_rate_ms);
        fflush(stdout);

        usleep(2000000); /* sample every 2 seconds */
    }

    sysop_uninit();
    return 0;
}

