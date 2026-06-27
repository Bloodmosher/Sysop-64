/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 *
 * sysop_sys — execute a SYS call on the C64 at a given address.
 *
 * Usage:  sysop_sys [-h] <addr>
 *   -h    interpret <addr> as hexadecimal (default is decimal)
 *
 * Acquires the DMA bus lock, executes sysop_sys(addr) on the C64, then
 * releases the bus and exits.  Equivalent to typing SYS <addr> in BASIC.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sysop64.h"

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("Usage: %s [-h (hexadecimal)] <addr>\n", argv[0]);
        return -1;
    }

    /* Check for optional -h flag to select hexadecimal input. */
    int hex = (argc > 2 && argv[1][0] == '-' && strlen(argv[1]) == 2 && argv[1][1] == 'h');

    /* Default to the classic C64 BASIC warm-start address ($FCE2 / 64738). */
    uint16_t address = 64738;

    if (hex)
        address = (uint16_t)strtoll(argv[2], NULL, 16);
    else
        address = (uint16_t)strtoll(argv[1], NULL, 10);

    sysop_init();

    int res = sysop_server_connect();
    if (res != 0)
    {
        printf("sysop_server_connect failed\n");
        return -1;
    }

    /* Acquire the DMA bus lock before touching the C64 address space. */
    sysop_server_dma_lock2();
    sysop_dma_enable();

    printf("SYS %d ($%04X)\n", (int)address, address);
    sysop_sys(address);

    /* Release the bus and clean up. */
    sysop_dma_disable();
    sysop_server_dma_unlock();
    sysop_server_disconnect();
    sysop_uninit();

    return 0;
}

