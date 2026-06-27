/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

/*
 * peek — read one or more bytes from C64 memory and print them.
 *
 * Acquires the DMA bus, reads the requested bytes via sysop_peek(), and prints
 * them as a hex dump (16 bytes per line, address-prefixed).  When a single
 * byte is read its binary representation is also printed.
 *
 * Usage:
 *   peek <address-hex> [count] [repeat] [compare-byte-hex]
 *
 *   address      C64 memory address in hex (e.g. d400).
 *   count        Number of consecutive bytes to read (default 1).
 *   repeat       Read the range this many times total (default 1).
 *                All repeat reads feed the mismatch counter but only the
 *                first pass is displayed.  Useful for stability testing.
 *   compare-byte Expected value in hex; a mismatch counter is printed
 *                after all reads when this argument is supplied.
 *
 * Examples:
 *   peek d400          Read one byte at $D400 (SID voice 1 freq lo)
 *   peek 0400 40       Hex-dump 64 bytes starting at $0400
 *   peek c6 1 100 00   Read $00C6 100 times, count how often it is non-zero
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "sysop64.h"
#include "sysop_defines.h"

int main(int argc, char **argv)
{
    uint8_t read_data = 0;
    uint8_t buffer[64 * 1024];  /* large enough for the full C64 address space */
    int buffer_index = 0;

    uint8_t compare_byte = 0;
    int count = 1;
    int i = 0, j = 0;
    int repeat = 1;
    int wrong = 0;  /* number of reads that did not match compare_byte */
    int a;

    if (argc < 2)
    {
        perror("Expected arguments: <address-hex> <count> [repeat] [compare-byte]");
        return -1;
    }

    a = strtoll(argv[1], NULL, 16);
    if (argc >= 3)
    {
        count = strtol(argv[2], NULL, 10);
    }

    if (argc >= 4)
    {
        repeat = strtol(argv[3], NULL, 10);
        printf("Using repeat of %d\n", repeat);
    }

    if (argc >= 5)
    {
        compare_byte = (uint8_t)strtol(argv[4], NULL, 16);
        printf("Using compare byte of %x\n", compare_byte);
    }

    sysop_init();

    int res = sysop_server_connect();
    if (res != 0)
    {
        printf("sysop_server_connect failed\n");
        sysop_uninit();
        return -1;
    }

    /* Hold the DMA bus for the entire read sequence so that the C64 CPU
     * cannot modify memory between individual sysop_peek() calls. */
    sysop_server_dma_lock2();

    /* Read count bytes, repeat times.  All results go into the flat buffer
     * so the mismatch counter covers every read.  Only the first pass
     * (buffer[0..count-1]) is displayed below. */
    for (j = 0; j < repeat; j++)
    {
        for (i = 0; i < count; i++)
        {
            buffer[buffer_index] = sysop_peek((uint16_t)a + i);
            if (buffer[buffer_index] != compare_byte)
            {
                wrong++;
            }
            buffer_index++;
        }
    }

    /* Print the first-pass results as a hex dump: 16 bytes per line,
     * each line prefixed with the C64 address of the first byte on that line. */
    j = 0;
    for (i = 0; i < count; i++)
    {
        if (j == 0)
        {
            printf("%04x: ", (uint16_t)(a + i));
        }

        printf("%02x ", buffer[i]);

        /* For a single-byte read, also show the value in binary — handy
         * when inspecting control/status registers bit by bit. */
        if (count == 1)
        {
            printByteAsBinary(buffer[i]);
        }
        j++;
        if (j == 16)
            j = 0;

        if ((i + 1) % 16 == 0 || i == count - 1)
        {
            printf("\n");
        }
    }

    /* Report how many reads across all repeat passes did not match the
     * expected byte (only meaningful when compare-byte was supplied). */
    if (argc >= 5)
    {
        printf("\nWrong count: %d\n", wrong);
    }

    sysop_server_dma_unlock();
    sysop_server_disconnect();
    sysop_uninit();

    return 0;
}
