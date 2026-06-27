/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * pll_config — HDMI pixel clock PLL synchronisation tool.
 *
 * Purpose
 * -------
 * The FPGA generates the HDMI pixel clock from an on-chip PLL fed by a
 * 50 MHz reference oscillator.  For a tear-free picture the pixel clock
 * must be locked to the C64's actual video frame rate, which in turn is
 * derived from the C64 CPU clock.  Because real C64 crystals run
 * slightly fast or slow compared to their nominal spec, a fixed pixel
 * clock would produce a slowly drifting beat between the C64 video
 * signal and the HDMI output.
 *
 * This tool:
 *   1. Reads the C64 CPU frequency as measured by the FPGA.
 *   2. Computes how far it deviates from the VIC chip's nominal spec.
 *   3. Scales the standard HDMI pixel clock (148.5 MHz) by the same
 *      ratio to produce a target pixel clock that exactly tracks the
 *      C64's actual frame rate.
 *   4. Solves for integer M / output-divider C / fractional K values
 *      that approximate the target on the FPGA PLL (50 MHz reference).
 *   5. Writes those values into the live PLL via the sysop library and
 *      triggers a PLL reconfiguration.
 *
 * PLL formula
 * -----------
 *   fout = fref * (M + K / 2^32) / C
 *
 * where:
 *   fref = 50 MHz (fixed reference)
 *   M    = integer multiplier  (8 ≤ M ≤ 25, keeps VCO in 400–1300 MHz)
 *   C    = integer output divider
 *   K    = 32-bit fractional numerator (0 = no fraction, 0xFFFFFFFF ≈ 1.0)
 *
 * Usage
 * -----
 *   pll_config [freq|0] [loop [seconds]] [onchange]
 *
 *   freq     — override the computed target frequency (MHz); 0 = auto
 *   loop     — keep running, reconfiguring every <seconds> (default 60)
 *   onchange — only reconfigure when the CPU frequency actually changes
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
#include "pll_calc.h"

/* Indices into pll_config[] for the three words that change each reconfiguration.
 * All other entries in the array are static PLL settings (N counter, unused C
 * outputs, charge pump, bandwidth) and are sent unchanged every time. */
#define M_VALUE_INDEX 1    // word 1  — M counter integer register
#define C_VALUE_INDEX 5    // word 5  — C0 output counter register
#define K_VALUE_INDEX 45   // word 45 — K fractional register

/* Full PLL register image — 46 32-bit words sent to the FPGA PLL
 * reconfiguration interface via sysop_set_pll_data().
 *
 * Only words at M_VALUE_INDEX, C_VALUE_INDEX, and K_VALUE_INDEX are
 * rewritten on each run; the rest encode fixed settings for the N
 * pre-divider (N=1), unused C1–C17 output counters, charge pump
 * current, and loop-filter bandwidth.
 *
 * Register encoding follows Intel (Altera) ALTPLL_RECONFIG IP specs.
 * The even-indexed word of each counter pair is an address/command
 * word; the odd-indexed word is the data value. */
uint32_t pll_config[] = {
    0b00000000000000000000000000000100,        // M COUNTER
    0b00000000000000000000010000000100,
    0b00000000000000000000000000000011,        // N COUNTER
    0b00000000000000010000000000000000,
    0b00000000000000000000000000000101,        // C0 COUNTER
    0b00000000000000100000001000000001,
    0b00000000000000000000000000000101,        // C1 COUNTER
    0b00000000000001010000000100000001,
    0b00000000000000000000000000000101,        // C2 COUNTER
    0b00000000000010010000000100000001,
    0b00000000000000000000000000000101,        // C3 COUNTER
    0b00000000000011010000000100000001,
    0b00000000000000000000000000000101,        // C4 COUNTER
    0b00000000000100010000000100000001,
    0b00000000000000000000000000000101,        // C5 COUNTER
    0b00000000000101010000000100000001,
    0b00000000000000000000000000000101,        // C6 COUNTER
    0b00000000000110010000000100000001,
    0b00000000000000000000000000000101,        // C7 COUNTER
    0b00000000000111010000000100000001,
    0b00000000000000000000000000000101,        // C8 COUNTER
    0b00000000001000010000000100000001,
    0b00000000000000000000000000000101,        // C9 COUNTER
    0b00000000001001010000000100000001,
    0b00000000000000000000000000000101,        // C10 COUNTER
    0b00000000001010010000000100000001,
    0b00000000000000000000000000000101,        // C11 COUNTER
    0b00000000001011010000000100000001,
    0b00000000000000000000000000000101,        // C12 COUNTER
    0b00000000001100010000000100000001,
    0b00000000000000000000000000000101,        // C13 COUNTER
    0b00000000001101010000000100000001,
    0b00000000000000000000000000000101,        // C14 COUNTER
    0b00000000001110010000000100000001,
    0b00000000000000000000000000000101,        // C15 COUNTER
    0b00000000001111010000000100000001,
    0b00000000000000000000000000000101,        // C16 COUNTER
    0b00000000010000010000000100000001,
    0b00000000000000000000000000000101,        // C17 COUNTER
    0b00000000010001010000000100000001,
    0b00000000000000000000000000001001,        // CHARGE PUMP
    0b00000000000000000000000000000010,
    0b00000000000000000000000000001000,        // BANDWIDTH SETTING
    0b00000000000000000000000000000111,
    0b00000000000000000000000000000111,        // M COUNTER FRACTIONAL VALUE
    0b11101110101001000011101000000111
    //0b11101110100111101110000010101011
};

/* bcd_to_binary() — convert a BCD-encoded integer to binary.
 *
 * The FPGA returns the measured C64 CPU frequency as a packed BCD value
 * (each nibble is one decimal digit, e.g. 0x985248 → 985248 Hz).
 * This function unpacks it into a plain uint32_t. */
uint32_t bcd_to_binary(uint32_t bcd)
{
    uint32_t result = 0;
    uint32_t multiplier = 1;

    while (bcd > 0) {
        uint8_t digit = bcd & 0xF;  // Extract lowest BCD digit (4 bits)
        result += digit * multiplier;

        multiplier *= 10;
        bcd >>= 4;  // Shift to the next digit
    }

    return result;
}
#define LINE_COUNT 46      // total number of 32-bit words in pll_config[]
#define MAX_LINE_LEN 255

/* encode_m_integer() — encode an integer counter value into the
 * Intel ALTPLL_RECONFIG register format.
 *
 * The PLL hardware divides the VCO by alternating between a HIGH
 * count and a LOW count each cycle.  For even divisors HIGH == LOW
 * == value/2.  For odd divisors (e.g. divide-by-3) one phase must
 * be one cycle longer: bit 17 (ODD_DIV) tells the hardware to add
 * one extra HIGH cycle, producing a 50% duty-cycle approximation.
 *
 * Register layout (bits):
 *   [15:8]  HIGH count (ceil(value/2))
 *   [ 7:0]  LOW  count (floor(value/2))
 *   [17]    ODD_DIV flag (1 when value is odd)
 *   [16]    BYPASS flag  (0 = counter active)
 *
 * This function is used for both M (VCO multiplier) and C (output
 * divider) since their register formats are identical. */
uint32_t encode_m_integer(uint16_t m_int_value) {
    uint8_t high_count = m_int_value / 2;       // HIGH phase: ≥ LOW by 1 if odd
    uint8_t low_count  = m_int_value - high_count; // LOW phase

    uint32_t m_reg = ((uint32_t)low_count << 8) | (uint32_t)high_count;

    // If M is odd, set bit 17 (odd division flag)
    if (m_int_value % 2 != 0) {
        m_reg |= (1 << 17);
    }

    // Bit 16 (bypass) remains 0

    return m_reg;
}

// Comparison function for qsort
int compare_uint32(const void *a, const void *b) {
    uint32_t arg1 = *(const uint32_t *)a;
    uint32_t arg2 = *(const uint32_t *)b;
    return (arg1 > arg2) - (arg1 < arg2);
}

int main(int argc, char **argv) 
{
    if (argc > 1 && strcmp(argv[1], "--h") == 0)
    {
        printf("Expected arguments: <override-frequency>|0 [loop [seconds]] [onchange]\n");
        return 0;
    }

    int loop = 0;
    if (argc > 2 && strcmp(argv[2], "loop") == 0)
    {
        loop = 1;
    }
    int loop_seconds = 60;
    if (argc > 3) 
    {
        loop_seconds = strtol(argv[3], NULL, 10);
    }
    if (loop)
        printf("Using loop time of %d seconds\n", loop_seconds);

    int onchange = 0;
    if (argc > 4 && strcmp(argv[4], "onchange") == 0)
    {
        printf("Reconfig only on CPU changes.\n");
        onchange = 1;
    }

    sysop_init();

    uint8_t vic_info = sysop_get_vic_info();
    if (!(vic_info & 0x80)) {
        printf("FPGA has not yet determined VIC model, aborting...\n");
        sysop_uninit();
        return -1;
    }

    double c64_frame_rate_spec = 50.12454212; // PAL
    double cpu_spec = 985248.0;
    double hdmi_hz = 50.0;
    switch (vic_info & 0x7)
    {
        case VIC_CHIP_6567R56A:
            c64_frame_rate_spec = 60.99278387; // NTSC "old"
            cpu_spec = 1022727.0;
            hdmi_hz = 60.0;
            break;
        case VIC_CHIP_6567R8:
            c64_frame_rate_spec = 59.8260895; // NTSC "new"
            cpu_spec = 1022727.0;
            hdmi_hz = 60.0;
            break;
        case VIC_CHIP_6572RO_DREAN:
            c64_frame_rate_spec = 50.46563116;
            cpu_spec = 1023443;
            break;
        case VIC_CHIP_6569:
        default:
            break;
    }

    int iteration = 1;
    uint32_t c64cpu_prev = 0;
    while(1)
    {
        printf("Using target frame rate %f and CPU spec %f\n", c64_frame_rate_spec, cpu_spec);

        uint32_t c64cpu = bcd_to_binary(sysop_get_cpu_freq());

        if (c64cpu == c64cpu_prev && onchange)
        {
            printf("C64 CPU unchanged, skipping PLL config\n");
            sleep(loop_seconds);
            continue;
        }

        if (c64cpu < 900000) { 
            printf("C64 not running, aborting PLL config...\n");
            if (loop) 
            {
                sleep(loop_seconds);
                continue;
            }
            
            sysop_uninit();
            return -1;
        }
        printf("CPU frequency: %u\n", c64cpu);
        c64cpu_prev = c64cpu;

    /* Frequency derivation:
     *
     * 1. Measure actual C64 CPU clock (FPGA returns BCD).
     * 2. ratio = actual_cpu / cpu_spec
     *    This captures any crystal deviation from nominal.
     * 3. actual_frame_rate = spec_frame_rate * ratio
     *    The C64 frame rate scales 1:1 with the CPU clock.
     * 4. video_freq_ratio = actual_frame_rate / hdmi_hz
     *    How far the C64 frame rate is from the HDMI base rate.
     * 5. target_pixel_clock = 148.5 MHz * video_freq_ratio
     *    Standard HDMI 1080p pixel clock scaled by the same ratio,
     *    so the HDMI frame rate matches the C64 frame rate exactly.
     */
    double ratio = (double)c64cpu / cpu_spec;
    double c64_frame_rate_actual = c64_frame_rate_spec * ratio;
    double video_freq_ratio = c64_frame_rate_actual / hdmi_hz;
    double target_freq = 148.5 * video_freq_ratio;
    printf("Ratio %f, target freq %f\n", video_freq_ratio, target_freq);

        if (argc > 1) {
            char *endptr;
            double ovr_freq = strtod(argv[1], &endptr);

            if (*endptr != '\0' || ovr_freq < 0) {
                fprintf(stderr, "Error: Invalid frequency format '%s'. Please provide a positive number.\n", argv[1]);
                goto exit;
            }
            if (ovr_freq != 0) 
            {
                printf("Command line override target %f versus calculated %f\n", ovr_freq, target_freq);
                target_freq = ovr_freq;
            }
        }


        double scaled = target_freq * pow(10, 6);
        // Round to nearest integer to avoid truncation error
        unsigned long result = llround(scaled);

        struct pll_calc_struct pll1;
        pll1.desired_frequency = result;
        printf("Target Freq: %u\n", pll1.desired_frequency);
        pll_calc_fixed(&pll1);

        uint32_t m_reg_value = encode_m_integer(pll1.m_value);
        uint32_t c_reg_value = encode_m_integer(pll1.c_value);

        printf("Set Freq: %u\n", pll1.desired_frequency);
        printf("M: %u, encoded as %08x\n", pll1.m_value, m_reg_value);
        printf("C: %u, encoded as %08X\n", pll1.c_value, c_reg_value);
        printf("K: %u\n", pll1.k_value);

        double m_frac = (double)pll1.k_value / (double)0xFFFFFFFF;
        double m_val = pll1.m_value + m_frac;
        double fout = 50.0 * m_val;
        fout /= (pll1.c_value * 1.0);
        printf("Reverse calc: m_frac %f,  freq %f\n", m_frac, fout);
        
        printf("M before: %08X, after %08X\n", pll_config[M_VALUE_INDEX], m_reg_value);
        printf("C before: %08X, after %08X\n", pll_config[C_VALUE_INDEX], c_reg_value);
        printf("K before: %08X, after %08X\n", pll_config[K_VALUE_INDEX], pll1.k_value);

        pll_config[M_VALUE_INDEX] = m_reg_value;
        pll_config[C_VALUE_INDEX] = c_reg_value;
        pll_config[K_VALUE_INDEX] = pll1.k_value;

        for (int i = 0; i < LINE_COUNT; i++) {
            sysop_set_pll_data(i, pll_config[i]);
        }
         
        sysop_pll_reconfig();
        printf("-- PLL reconfig #%d done--\n\n", iteration++);

        if (!loop)
            break;
        else
            sleep(loop_seconds);
    }

exit:
    sysop_uninit();

    return 0;
}
