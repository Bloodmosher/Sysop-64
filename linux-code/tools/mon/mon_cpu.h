/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * mon_cpu.h — CPU state display, single-stepping, and memory/file utilities.
 */

#pragma once

#include <stdint.h>

// Print the current CPU registers (reads from C64 stack frame).
// Updates the global g_pc and g_status caches.
void show_registers(void);

// Update the saved return address on the C64 stack so that when the CPU
// resumes it will execute at 'address'.
void setpc(uint16_t address);

// Resume the C64 from a breakpoint, patching up the PC and the BRK vector.
void resume(void);

// Determine the next PC value for single-step (accounts for branches/jumps).
// Returns 0 on success (pNextPc is filled), -1 if more bytes are needed.
int determine_next_pc(uint16_t *pNextPc);

// Type "RUN\r" into the C64 keyboard buffer.
void run(void);

// Peek a single byte, acquiring/releasing DMA around the read.
uint8_t Peek(uint16_t addr);

// Save a range of C64 memory to a file.
// If prg is non-zero, prepend a two-byte load address ($0108) header.
// Returns 0 on success, non-zero on error.
int save(uint16_t start_addr, uint16_t end_addr, const char *filename, int prg);

