/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * mon_private.h — shared includes, constants, globals, and the OwnDma RAII class.
 * Included by every mon/ translation unit.
 */

#pragma once

#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <ncurses.h>

#include "sysop64.h"
#include "instructions.h"
#include "asm.h"

// ---------------------------------------------------------------------------
// Input / command-line constants
// ---------------------------------------------------------------------------
#define MAX_BUFFER_SIZE   1024   // size of the main input line buffer
#define MAX_ARGUMENTS     20     // maximum tokens per command line
#define MAX_LINE_LENGTH   256    // max characters per input line (including NUL)
#define HISTORY_MAX_LINES 100    // number of entries in the command history ring
#define HISTORY_MAX_LEN   50     // max characters per history entry (including NUL)

// ---------------------------------------------------------------------------
// C64 debug protocol addresses (in the Sysop-64 I/O space)
// ---------------------------------------------------------------------------
#define STORED_STACK_ADDRESS     0xdff0
#define BREAKPOINT_COUNT_ADDRESS 0xdfe0
#define BREAKPOINT_TABLE_ADDRESS 0xdfe1
#define MAX_BREAKPOINTS          5

// ---------------------------------------------------------------------------
// Globals shared across modules (defined in mon.cpp)
// ---------------------------------------------------------------------------
extern uint16_t g_pc;        // cached program counter (updated by show_registers)
extern uint8_t  g_status;    // cached processor status register
extern int      own_dma;     // non-zero while this process holds the C64 bus
extern uint8_t *sysop64_cmd_address; // command register pointer (old-style SYSOP64_CMD_ID_* writes)

// ---------------------------------------------------------------------------
// OwnDma — RAII guard for C64 bus ownership.
//
// Acquires DMA on construction if not already owned; releases on destruction.
// Nesting is safe: only the outermost instance actually asserts/de-asserts DMA.
// ---------------------------------------------------------------------------
class OwnDma {
public:
    OwnDma()  : _had_dma(own_dma) { if (!own_dma) { sysop_dma_enable();  own_dma = 1; } }
    ~OwnDma()                     { if (!_had_dma) { sysop_dma_disable(); own_dma = 0; } }
private:
    int _had_dma;
};
