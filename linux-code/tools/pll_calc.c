/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include <stdio.h>
#include "pll_calc.h"

/*
 * pll_calc_fixed()
 *
 * Originally from:
 *   Intel meta-de10-nano
 *   examples/standalone/de10_nano_hdmi_config.c
 *
 * Copyright (c) 2017 Intel Corporation
 * Licensed under the MIT License.
 *
 * Source:
 * https://github.com/intel/meta-de10-nano/blob/master/recipes-bsp/u-boot/files/v2017.03/0005-Add-DE10-Nano-HDMI-configuration-and-debug-apps.patch
 *
 * Modifications:
 *   - Adapted for standalone use.
 *   - Removed U-Boot dependencies.
 *   - Simplified interface.
 */

/* Reference oscillator fed to the PLL (Hz). */
#define PLL_REF_FREQ    (50000000)

/* Valid range for the integer VCO multiplier M.
 * With a 50 MHz reference this keeps the VCO within 400–1300 MHz. */
#define MIN_MULTIPLE    (8)
#define MAX_MULTIPLE    (26)

/* 32-bit fixed-point helpers for fractional arithmetic.
 * Fractions below 0.05 are snapped to 0 (integer M), fractions above
 * 0.95 are rounded up (M + 1) to avoid marginal PLL fractions. */
#define FXD_PNT_1P0     (1ULL << 32)
#define FXD_PNT_0P95    ((unsigned long long)(FXD_PNT_1P0 * 0.95))
#define FXD_PNT_0P05    ((unsigned long long)(FXD_PNT_1P0 * 0.05))

void pll_calc_fixed(struct pll_calc_struct *the_pll_calc_struct)
{
    int i;
    unsigned long long input_freq;
    unsigned long long clock_ratio;
    unsigned long long min_div_ratio;
    unsigned long long ceil_min_div_ratio;
    unsigned long long product_ratio_ceil;
    unsigned long long floor_product;
    unsigned long long fraction;
    unsigned long long k_value;

    /*
     * Sweep upward in 100 Hz steps to escape any dead zones where no valid
     * M/C pair exists at the exact desired frequency.
     * Most dead zones are ≤ 100 kHz wide, so 1000 × 100 Hz covers them.
     */
    int maxTries = 1000;
    for (i = 0; i < maxTries; i++) {
        /* Nudge the target up by 100 Hz each pass.  The caller receives the
         * adjusted frequency so it knows exactly what was programmed. */
        input_freq = the_pll_calc_struct->desired_frequency + (i * 100);

        /* clock_ratio = target / fref, in 32-bit fixed-point. */
        clock_ratio = (input_freq << 32) / PLL_REF_FREQ;
        if (clock_ratio == 0) {
            the_pll_calc_struct->desired_frequency = 0;
            the_pll_calc_struct->m_value = 1;
            return;
        }

        min_div_ratio = ((unsigned long long)MIN_MULTIPLE << 32) / clock_ratio;
        ceil_min_div_ratio = min_div_ratio;
        if ((((unsigned long long)MIN_MULTIPLE << 32) % clock_ratio) > 3)
            ceil_min_div_ratio += 1;

        product_ratio_ceil = clock_ratio * ceil_min_div_ratio;

        fraction = 0;
        floor_product = 0;
        while (product_ratio_ceil < ((unsigned long long)MAX_MULTIPLE << 32)) {
            /* Extract integer M from the fixed-point product. */
            floor_product = product_ratio_ceil & 0xFFFFFFFF00000000ULL;
            fraction = product_ratio_ceil - floor_product;

            /* Accept fractions in [0.05, 0.95] as genuine K values. */
            if ((fraction >= (FXD_PNT_0P05 - 3)) &&
                (fraction <= (FXD_PNT_0P95 + 0)))
                break;

            /* Fraction very close to 0: use integer M with no fraction. */
            if (fraction <= 4) {
                fraction = 0;
                break;
            }

            /* Fraction very close to 1: round M up and use no fraction. */
            if (fraction >= 0xFFFFFFFC) {
                fraction = 0;
                floor_product += FXD_PNT_1P0;
                break;
            }

            ceil_min_div_ratio++;
            product_ratio_ceil = clock_ratio * ceil_min_div_ratio;
        }

        if (product_ratio_ceil >= ((unsigned long long)MAX_MULTIPLE << 32))
            continue; /* dead zone — try next frequency */
        else
            break;    /* valid solution found */
    }

    if (i >= maxTries) {
        the_pll_calc_struct->desired_frequency = 0;
        return;
    }

    k_value = (fraction == 0) ? 1 : fraction;

    printf("Values set, iterations: %d\n", i + 1);
    the_pll_calc_struct->desired_frequency = (unsigned long)input_freq;
    the_pll_calc_struct->m_value           = (unsigned long)(floor_product >> 32);
    the_pll_calc_struct->c_value           = (unsigned long)ceil_min_div_ratio;
    the_pll_calc_struct->k_value           = (unsigned long)k_value;
}
