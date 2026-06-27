/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include <error.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include "sysop64.h"

/*
 * dmatiming — calibrate or manually set the FPGA DMA write-timing window.
 *
 * Background
 * ----------
 * When the FPGA DMA engine writes a byte to the C64 expansion bus it must
 * drive the data lines during a precise sub-window of the C64 PHI2 clock
 * cycle.  The FPGA samples the bus at a rate much higher than PHI2 and
 * assigns each sample a "tick" counter that resets at the PHI2 rising edge.
 *
 * The DMA timing register holds two tick values:
 *   write_start_tick — the tick at which the FPGA begins driving the bus
 *   write_end_tick   — the tick at which the FPGA stops driving the bus
 *
 * These values are packed into a uint16: (write_end_tick << 8) | write_start_tick.
 *
 * If the window is set too early the data bus has not yet settled; too
 * late and the write misses the valid PHI2-high window.  The correct values
 * depend on the specific hardware (board capacitance, cable length, PAL vs
 * NTSC PHI2 timing), so they must be measured from a live C64.
 *
 * Auto-calibration
 * ----------------
 * calibrate_once() captures the C64 bus with the FPGA sampler, finds a
 * genuine CPU write cycle, and measures the tick boundaries of the write
 * window by observing when data becomes stable and when PHI2 falls.
 *
 * auto_calibrate() repeats this 100 times and takes the most common
 * (start, end) pair as the final value, providing robustness against
 * occasional noisy or mis-triggered samples.
 *
 * Usage
 * -----
 *   dmatiming auto                     — auto-calibrate for current VIC type
 *   dmatiming [pal|ntsc] <low> <high>  — set timing manually
 */

/* How many calibration captures to take; the mode of the results is used.
 * More runs = more robust against occasional bad captures. */
#define CALIBRATION_RUNS 100

/* Result of one successful calibration capture.
 *   write_start_tick — first sampler tick (within PHI2-high) at which the
 *                       data bus holds a stable value during a CPU write.
 *   write_end_tick   — last tick at which the data bus is valid before PHI2
 *                       falls at the end of the write cycle.
 *   valid            — 1 if the capture succeeded, 0 if it should be discarded. */
struct tick_pair
{
    uint8_t write_start_tick;
    uint8_t write_end_tick;
    int valid;
};

/* calibrate_once() — capture the C64 bus and measure one DMA write window.
 *
 * Algorithm:
 *   1. Start the FPGA sampler, wait 10 ms to fill the buffer with real
 *      C64 bus activity (12.5 M samples at the sampler clock rate).
 *   2. Scan forward until a CPU write cycle is found:
 *      BA=1, PHI2=1, R/W=0 (bus available, clock high, write direction).
 *   3. Wait for PHI2 to go low (end of that write cycle) so we start
 *      fresh at a clean PHI2 edge boundary.
 *   4. Find the *next* CPU write cycle with non-zero data.
 *   5. Scan forward to the PHI2 falling edge to locate write_end_tick
 *      (the last tick with valid data).
 *   6. Walk *backward* from the falling edge to find the first tick at
 *      which the data value matches the stable value, giving write_start_tick.
 *
 * Returns 1 and fills *result on success; returns 0 on failure. */
int calibrate_once(struct tick_pair *result)
{
    sysop_sampler_wait_not_busy();  // ensure any previous capture has completed
    sysop_sampler_start();           // arm the FPGA bus sampler
    usleep(10000);             // wait 10 ms to fill the capture buffer

    uint64_t raw = 0;
    struct sysop_c64_bus_sample sample;
    uint32_t i = 0;

    /* Step 2: find the first CPU write cycle (BA=1, PHI2=1, R/W=0). */
    int found_first = 0;
    for (i = 0; i < SYSOP64_SAMPLER_MAX_SAMPLES; i++)
    {
        sysop_sampler_get_sample(i, &sample);
        if (sample.ba == 1 && sample.phi2 == 1 && sample.r__w == 0)
        {
            found_first = 1;
            i++;
            break;
        }
    }
    if (!found_first)
    {
        printf("No matching sample, calibration failed\n");
        return 0;
    }
    /* Step 3: advance past the PHI2 falling edge so we start at a
     * clean cycle boundary before searching for the measurement sample. */
    int found_phi_lo = 0;
    for (; i < SYSOP64_SAMPLER_MAX_SAMPLES; i++)
    {
        sysop_sampler_get_sample(i, &sample);
        if (sample.phi2 == 0)
        {
            found_phi_lo = 1;
            i++;
            break;
        }
    }
    if (!found_phi_lo)
    {
        printf("Could not find next phi lo, calibration failed\n");
        return 0;
    }

    /* Step 4: find the next CPU write cycle with non-zero data.
     * Requiring data != 0 avoids latching on an undriven bus state. */
    int found_write_start = 0;
    uint8_t write_start_tick = 0;
    for (; i < SYSOP64_SAMPLER_MAX_SAMPLES; i++)
    {
        sysop_sampler_get_sample(i, &sample);

        if (sample.ba == 1 && sample.phi2 == 1 && sample.r__w == 0 && sample.data != 0)
        {
            found_write_start = 1;
            write_start_tick = sample.sample_tick;
            i++;
            break;
        }
    }
    if (!found_write_start)
    {
        printf("Could not find next write start, calibration failed\n");
        return 0;
    }

    /* Step 5: scan forward to the PHI2 falling edge; the sample just
     * before PHI2 falls is the last moment the data bus is valid.
     * Step back one sample to get the final stable data value and tick. */
    uint8_t write_end_tick = 0;
    found_phi_lo = 0;
    uint8_t expected_data_val = 0;
    for (; i < SYSOP64_SAMPLER_MAX_SAMPLES; i++)
    {
        sysop_sampler_get_sample(i, &sample);

        if (sample.phi2 == 0)
        {
            found_phi_lo = 1;
            printf("Found PHI2=0 at tick %d, using previous sample as write_end\n", sample.sample_tick);
            i--;
            sysop_sampler_get_sample(i, &sample);
            write_end_tick = sample.sample_tick;
            printf("Write end at tick %d, data value was %02X\n", sample.sample_tick, sample.data);
            expected_data_val = sample.data;
            break;
        }
    }
    /* Step 6: walk backward from the PHI2 falling edge to find the
     * first tick where the data bus held the stable write value.
     * This gives write_start_tick — the earliest tick at which data is valid. */
    for (; i > 0; i--)
    {
        sysop_sampler_get_sample(i, &sample);
        if (sample.data != expected_data_val || sample.phi2 == 0)
        {
            printf("First non matching data value or phi2=0 at tick %d\n", sample.sample_tick);
            write_start_tick = sample.sample_tick;
            break;
        }
        else
        {
            printf("Found expected sample data at tick %d\n", sample.sample_tick);
        }
    }
    if (i != 0 && write_end_tick > write_start_tick)
    {
        printf("Found write edges at %d and %d\n", write_start_tick, write_end_tick);
        result->write_start_tick = write_start_tick;
        result->write_end_tick = write_end_tick;
        result->valid = 1;
        return 1;
    }

    return 0;
}

/* auto_calibrate() — run CALIBRATION_RUNS captures and pick the best timing.
 *
 * Each capture returns a (write_start_tick, write_end_tick) pair.  Because
 * bus noise or an unfortunate sample window can occasionally produce an
 * outlier, we count how many times each distinct pair appears and use the
 * mode (most-common value) as the final result.
 *
 * A safety margin of -1 is subtracted from write_start_tick before
 * programming to ensure the FPGA begins driving the bus slightly before
 * the data is confirmed stable, covering any inter-run jitter.
 *
 * The result is written to the PAL or NTSC DMA timing register depending
 * on which VIC chip the FPGA has detected. */
void auto_calibrate()
{
    struct tick_pair results[CALIBRATION_RUNS];
    int successful_runs = 0;

    printf("Running calibration %d times...\n", CALIBRATION_RUNS);

    for (int run = 0; run < CALIBRATION_RUNS; run++)
    {
        printf("\n=== Calibration run %d/%d ===\n", run + 1, CALIBRATION_RUNS);
        if (calibrate_once(&results[successful_runs]))
        {
            successful_runs++;
        }
        else
        {
            printf("Calibration run %d failed\n", run + 1);
        }
    }

    if (successful_runs == 0)
    {
        printf("All calibration runs failed, cannot auto calibrate\n");
        return;
    }

    printf("\n=== Finding most common value pair ===\n");
    printf("Successful calibration runs: %d/%d\n", successful_runs, CALIBRATION_RUNS);

    /* Mode-find: count occurrences of each (start, end) pair and keep
     * the one that appeared most often across all successful runs. */
    int max_count = 0;
    uint8_t best_write_start_tick = 0;
    uint8_t best_write_end_tick = 0;

    for (int i = 0; i < successful_runs; i++)
    {
        int count = 0;
        for (int j = 0; j < successful_runs; j++)
        {
            if (results[i].write_start_tick == results[j].write_start_tick &&
                results[i].write_end_tick == results[j].write_end_tick)
            {
                count++;
            }
        }

        printf("Pair (%d, %d) occurred %d times\n",
               results[i].write_start_tick, results[i].write_end_tick, count);

        if (count > max_count)
        {
            max_count = count;
            best_write_start_tick = results[i].write_start_tick;
            best_write_end_tick = results[i].write_end_tick;
        }
    }

    printf("\nMost common pair: (%d, %d) occurred %d times\n",
           best_write_start_tick, best_write_end_tick, max_count);

    /* Subtract 1 from start tick as a safety margin so the FPGA begins
     * asserting DMA data one tick earlier than the measured stable point,
     * guarding against any slight jitter between runs. */
    best_write_start_tick--;

    printf("Using %d and %d\n", best_write_start_tick, best_write_end_tick);

    /* Pack both ticks into one 16-bit value: high byte = end, low byte = start. */
    uint16_t combined = (uint16_t)(best_write_end_tick << 8) | best_write_start_tick;

    printf("Setting to %04X\n", combined);

    /* Write to the PAL or NTSC timing register — the FPGA maintains
     * separate values because PHI2 timing differs between the two standards. */
    if (sysop_is_pal())
        sysop_set_dma_timing_pal(combined);
    else
        sysop_set_dma_timing_ntsc(combined);

    printf("Done.\n");
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "auto") == 0)
    {
        sysop_init();
        auto_calibrate();
        sysop_uninit();
        return 0;
    }

    if (argc != 4)
    {
        printf("Expected arguments: [ntsc|pal low-index high-index] | [auto]\n");
        return 0;
    }

    int is_pal = 0;
    if (strcasecmp(argv[1], "pal") == 0)
        is_pal = 1;
    else if (strcasecmp(argv[1], "ntsc") != 0)
    {
        printf("Must be either 'pal' or 'ntsc'\n");
        return 0;
    }

    /* Manual mode: pack the user-supplied tick indices and write directly.
     * low = write_start_tick, high = write_end_tick.
     * The combined value is (high << 8) | low, matching auto_calibrate's layout. */
    uint8_t low = (uint8_t)strtol(argv[2], NULL, 10);
    uint8_t high = (uint8_t)strtol(argv[3], NULL, 10);
    uint16_t combined = (uint16_t)(high << 8) | low;

    printf("Setting to %04X\n", combined);

    sysop_init();

    if (is_pal)
        sysop_set_dma_timing_pal(combined);
    else
        sysop_set_dma_timing_ntsc(combined);

    sysop_uninit();

    return 0;
}
