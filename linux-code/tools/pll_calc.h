/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * pll_calc.h — FPGA PLL M/C/K solver.
 *
 * Solves for the integer multiplier (M), integer output divider (C), and
 * 32-bit fractional value (K) that make an Intel/Altera ALTPLL produce a
 * requested output frequency from a 50 MHz reference:
 *
 *   fout = fref * (M + K / 2^32) / C
 *
 * See pll_calc.c for the full algorithm description.
 */

#ifndef PLL_CALC_H
#define PLL_CALC_H

/*
 * pll_calc_struct — input/output for pll_calc_fixed().
 *
 *   desired_frequency  in:  target fout in Hz
 *                      out: actual frequency achieved (may differ by ≤100 kHz
 *                           if the exact value fell in a dead zone); set to 0
 *                           on failure.
 *   m_value            out: integer M multiplier
 *   c_value            out: integer C output divider
 *   k_value            out: 32-bit fractional K (1 = no fraction)
 */
struct pll_calc_struct {
    unsigned long desired_frequency;
    unsigned long m_value;
    unsigned long c_value;
    unsigned long k_value;
};

/* ── Function declaration ───────────────────────────────────────────────── */

/*
 * pll_calc_fixed — solve for M, C, K given a target output frequency.
 *
 * Fills the_pll_calc_struct with valid register values or sets
 * desired_frequency to 0 on failure (no solution found within 1000 attempts).
 */
void pll_calc_fixed(struct pll_calc_struct *the_pll_calc_struct);

#endif /* PLL_CALC_H */
