/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * mon_breakpoints.h — breakpoint table management.
 *
 * Breakpoints are stored both in this process and mirrored into C64 RAM
 * so the on-C64 monitor stub can reference them.  The table holds the
 * original opcode that was replaced by a BRK ($00) instruction.
 */

#pragma once

#include "mon_private.h"

struct breakpoint {
    uint16_t address;
    uint8_t  opcode;
} __attribute__((packed));

extern struct breakpoint g_breakpoints[MAX_BREAKPOINTS];
extern uint8_t g_num_breakpoints;
extern int     g_breakpoint_refresh_needed;

// Sync the in-memory table from C64 RAM.
void read_breakpoints(void);

// Write the in-memory table to C64 RAM.
void write_breakpoints(void);

// Read breakpoints from C64 RAM if a refresh is pending (or force=1).
void refresh_breakpoints_if_needed(int force);

// Set a breakpoint at address: saves the original opcode, pokes BRK, updates table.
void set_breakpoint(uint16_t address);

// Remove a specific breakpoint and restore its original opcode.
void remove_breakpoint(uint16_t address);

// Clear all breakpoints.  If restore_opcodes is non-zero, the original opcodes
// are written back to C64 RAM before the table is cleared.
void clear_breakpoints(int restore_opcodes);

// Print all active breakpoints.
void show_breakpoints(void);

// Return the saved opcode for the breakpoint at address, or -1 if none.
int is_breakpoint(uint16_t address);
