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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "sysop64.h"
#include "sysop_defines.h"

/*
 * sysop_poke — write one or more bytes into C64 memory via FPGA DMA.
 *
 * Usage
 * -----
 *   sysop_poke <addr-hex> <data-hex> [count] [repeat]
 *   sysop_poke <addr-hex> %<data-binary>
 *   sysop_poke --hex <addr-hex> <b0> <b1> ... <bN>
 *
 * Modes
 * -----
 * Default mode:
 *   Writes a single byte value to a starting address.
 *   If <count> > 1, writes to addr, addr+1, ..., addr+count-1 (same value).
 *   If <repeat> > 1, loops the count-write that many times, incrementing
 *   the byte value by 1 each iteration (useful for gradient/sweep patterns).
 *
 *   Data can be given as hexadecimal (e.g. FF) or binary with a '%' prefix
 *   (e.g. %11001100).
 *
 * --hex mode:
 *   Writes a sequence of individual hex bytes starting at <addr>.
 *   Each byte on the command line is poked to the next consecutive address.
 *   Useful for writing small code or data sequences in one command.
 *
 * DMA locking
 * -----------
 * All sysop_poke calls are wrapped in sysop_server_dma_lock / sysop_server_dma_unlock to freeze
 * the C64 CPU on the bus while writing, preventing bus conflicts.
 * sysop_dma_wait_empty() + sysop_dma_wait_not_busy() are called before releasing the
 * lock so all queued writes have fully committed before the CPU resumes.
 */

int main(int argc, char **argv)
{
    uint64_t a = 0, b = 0;

    int count = 1;
    int i = 0, j = 0;
    int repeat = 1;

    if (argc < 3)
    {
        perror("Expected arguments: <address-hex> [%<data-bin>]|<data-hex> <count>");
        perror("Or --hex <addr> <b0> <b1> ... <bn> bytes in hex\n");
        return -1;
    }
    sysop_init();

    int res = sysop_server_connect();
    if (res != 0)
    {
        printf("sysop_server_connect failed\n");
        return -1;
    }

    /* --hex mode: write a variable-length byte sequence starting at addr. */
    if (argc > 2 && strcmp(argv[1], "--hex") == 0)
    {
        sysop_server_dma_lock();
        a = strtoll(argv[2], NULL, 16);
        int cnt = 0;
        for (int i = 3; i < argc; i++)
        {
            uint16_t addr = (uint16_t)a + cnt;
            unsigned int byte_val;
            if (sscanf(argv[i], "%x", &byte_val) == 1)
            {
                printf("%04X: %02X\n", addr, (uint8_t)byte_val);
                sysop_poke(addr, (uint8_t)byte_val);
                cnt++;
            }
        }
        printf("Poked %d bytes\n", cnt);
        sysop_server_dma_unlock();
        return 0;
    }

    /* Parse address (always hex). */
    a = strtoll(argv[1], NULL, 16);

    /* Parse data value: '%' prefix means binary, otherwise hex. */
    if (argv[2][0] == '%')
    {
        b = strtoll(&argv[2][1], NULL, 2);
    }
    else
    {
        b = strtoll(argv[2], NULL, 16);
    }

    /* Optional count: number of consecutive addresses to write (default 1). */
    if (argc >= 4)
    {
        count = strtol(argv[3], NULL, 10);
    }

    /* Optional repeat: number of times to redo the count-write (default 1).
     * Each repeat increments the byte value by 1, so e.g. repeat=3 writes
     * value b at the range, then b+1, then b+2. */
    if (argc >= 5)
    {
        repeat = strtol(argv[4], NULL, 10);
        printf("Using repeat of %d\n", repeat);
    }

    printf("%x: %x ", (int)a, (int)b);
    printByteAsBinary(b);
    printf("\n");

    /* Freeze the C64 CPU via DMA while writing to prevent bus conflicts. */
    sysop_server_dma_lock2();
    for (j = 0; j < repeat; j++)
    {
        printf("iteration %d\n", j + 1);

        for (i = 0; i < count; i++)
        {
            sysop_poke((uint16_t)a + i, (uint8_t)b + j);
        }
    }

    /* Wait for all queued DMA writes to complete before releasing the bus. */
    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();

    sysop_server_dma_unlock();
    sysop_server_disconnect();
    sysop_uninit();
    return 0;
}
