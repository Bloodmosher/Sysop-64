/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "mon_private.h"
#include "mon_breakpoints.h"

// ---------------------------------------------------------------------------
// Breakpoint table
// ---------------------------------------------------------------------------

struct breakpoint g_breakpoints[MAX_BREAKPOINTS];
uint8_t g_num_breakpoints = 0;
int g_breakpoint_refresh_needed = 1;

// ---------------------------------------------------------------------------

void read_breakpoints(void)
{
    OwnDma odma;
    g_num_breakpoints = sysop_peek(BREAKPOINT_COUNT_ADDRESS);
    unsigned char *ptr = (unsigned char *)&g_breakpoints;
    for (int i = 0; i < (int)sizeof(g_breakpoints); i++)
        ptr[i] = sysop_peek(BREAKPOINT_TABLE_ADDRESS + i);
}

void write_breakpoints(void)
{
    OwnDma odma;
    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();

    sysop_poke(BREAKPOINT_COUNT_ADDRESS, g_num_breakpoints);
    unsigned char *ptr = (unsigned char *)&g_breakpoints;
    for (int i = 0; i < (int)sizeof(g_breakpoints); i++)
        sysop_poke(BREAKPOINT_TABLE_ADDRESS + i, ptr[i]);
}

void refresh_breakpoints_if_needed(int force)
{
    if (force || g_breakpoint_refresh_needed)
        read_breakpoints();
    g_breakpoint_refresh_needed = 0;
}

void set_breakpoint(uint16_t bp_address)
{
    OwnDma odma;
    read_breakpoints();
    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();

    if (g_num_breakpoints == MAX_BREAKPOINTS) {
        printf("No breakpoints available.\n");
        return;
    }

    // Find an empty slot
    int index = -1;
    for (int i = 0; i < MAX_BREAKPOINTS; i++) {
        if (g_breakpoints[i].address == 0) {
            index = i;
            break;
        }
    }
    if (index == -1) {
        printf("No slot for breakpoint\n");
        return;
    }

    uint8_t opcode = sysop_peek(bp_address);
    g_breakpoints[index].address = bp_address;
    g_breakpoints[index].opcode  = opcode;
    sysop_poke(bp_address, 0x00);  // BRK
    g_num_breakpoints++;
    write_breakpoints();
    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();
}

void remove_breakpoint(uint16_t bp_address)
{
    OwnDma odma;
    read_breakpoints();
    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();

    int found = 0;
    for (int i = 0; i < MAX_BREAKPOINTS; i++) {
        if (g_breakpoints[i].address == bp_address) {
            sysop_poke(g_breakpoints[i].address, g_breakpoints[i].opcode);
            g_breakpoints[i].address = 0;
            g_breakpoints[i].opcode  = 0;
            found = 1;
        }
    }

    if (found) {
        write_breakpoints();
        sysop_dma_wait_empty();
        sysop_dma_wait_not_busy();
    } else {
        printf("No breakpoint found at $%04X\n", bp_address);
    }
}

void clear_breakpoints(int restore_opcodes)
{
    OwnDma odma;
    read_breakpoints();
    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();

    for (int i = 0; i < MAX_BREAKPOINTS; i++) {
        if (g_breakpoints[i].address != 0) {
            if (restore_opcodes)
                sysop_poke(g_breakpoints[i].address, g_breakpoints[i].opcode);
            g_breakpoints[i].address = 0;
            g_breakpoints[i].opcode  = 0;
        }
    }

    write_breakpoints();
    sysop_poke(BREAKPOINT_COUNT_ADDRESS, 0x00);
    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();
}

void show_breakpoints(void)
{
    read_breakpoints();
    for (int i = 0; i < MAX_BREAKPOINTS; i++) {
        if (g_breakpoints[i].address != 0)
            printf("BP %d: %04X (%02X)\n", i,
                   g_breakpoints[i].address, g_breakpoints[i].opcode);
    }
}

int is_breakpoint(uint16_t address)
{
    for (int i = 0; i < MAX_BREAKPOINTS; i++) {
        if (g_breakpoints[i].address == address)
            return g_breakpoints[i].opcode;
    }
    return -1;
}
