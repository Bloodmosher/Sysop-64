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
#include <sys/mman.h>
#include <unistd.h>
#include "sysop64.h"

int main(int argc, char **argv)
{
    uint64_t a = 0;
    uint64_t b = 0;
    uint64_t sum = 0;
    uint64_t start_timestamp = 0;
    uint64_t timestamp = 0;

    uint8_t read_data = 0;
    uint8_t buffer[64 * 1024];
    int buffer_index = 0;

    uint8_t compare_byte = 0;

    int count = 1;
    int i = 0, j = 0;
    int repeat = 1;
    int wrong = 0;

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

    // printf("%x: %x\n", (int)a, (int)b);

    sysop_init();

    uint64_t start = sysop_phi2_counter();
    for (j = 0; j < repeat; j++)
    {
        for (i = 0; i < count; i++)
        {
            buffer[buffer_index] = sysop_internal_peek((uint16_t)a + i);
            if (buffer[buffer_index] != compare_byte)
            {
                wrong++;
            }
            buffer_index++;
        }
        //	  usleep(100000);
    }
    uint64_t end = sysop_phi2_counter();
    j = 0;
    for (i = 0; i < count; i++)
    {
        if (j == 0)
        {
            printf("%04x: ", (uint16_t)(a + i));
        }

        printf("%02x ", buffer[i]);
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

    if (argc >= 5)
    {
        printf("\nWrong count: %d\n", wrong);
    }

    printf("%llu C64 cycles\n", end - start);
    sysop_uninit();
    return 0;
}
