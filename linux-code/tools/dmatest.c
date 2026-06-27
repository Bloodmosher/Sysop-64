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
#include <time.h>
#include <math.h>
#include <signal.h>
#include "sysop64.h"

void sigintHandler(int signal)
{
    sysop_poke(0xd020, 0);
    sysop_poke(0xd021, 0);
    usleep(50000);
    sysop_server_dma_unlock();
    sysop_server_disconnect();
    sysop_uninit();
    exit(signal);
}

uint8_t g_cyclesPerLine = 63;
uint16_t g_vic_lines = 312;

int main(int argc, char **argv)
{
    signal(SIGINT, sigintHandler);

    sysop_init();

    uint8_t vic_info = sysop_get_vic_info();
    if (!(vic_info & 0x80))
    {
        printf("FPGA has not yet determined VIC model.\n");
        return -1;
    }
    else
    {
        printf("VIC Model: ");
        switch (vic_info & 0x7)
        {
        case VIC_CHIP_6567R56A:
        {
            printf("NTSC OLD");
            g_cyclesPerLine = 64;
            g_vic_lines = 262;
        }
        break;
        case VIC_CHIP_6567R8:
        {
            printf("NTSC NEW");
            g_cyclesPerLine = 65;
            g_vic_lines = 263;
        }
        break;
        case VIC_CHIP_6572RO_DREAN:
        {
            printf("DREAN");
            g_cyclesPerLine = 65;
            g_vic_lines = 312;
        }
        break;
        case VIC_CHIP_6569:
        {
            printf("PAL");
            g_cyclesPerLine = 63;
            g_vic_lines = 312;
        }
        break;

        default:
        {
            printf("Unknown");
        }
        break;
        }
        printf("\n");
    }
    int res = sysop_server_connect();
    if (res == -1)
    {
        printf("sysop_server_connect() failed\n");
        return -1;
    }

    sysop_server_dma_lock();

    sysop_poke(0xd020, 1);

    int i = 0;
    int waitFirst = 52;
    int count = 1;
    int waitLine = waitFirst;
    int prevWaitLine = 0;
    uint8_t waitChar = 0;
    uint8_t color = 2; 

    sysop_poke(0xd016, 0xc8);
    sysop_poke(0xd011, 0x1b);

    count = 1;
    waitChar = 12; 
    while (1)
    {
        if (waitLine >= 251 || waitLine < 51)
        {
            sysop_wait_vic2(waitLine, waitChar);
            for (i = 0; i < 5; i++)
            {
                sysop_poke(0xd020, color + i);
            }
            for (i = 0; i < 40; i++)
            {
                sysop_poke(0xd020, color + 5 + i);
            }
            for (i = 0; i < 4; i++)
            {
                sysop_poke(0xd020, color + 14 + i);
            }
            sysop_poke(0xd020, 0);

            waitLine += 1;
        }
        else if (waitLine >= waitFirst && waitLine <= 251)
        {
            sysop_wait_vic2(waitLine, waitChar);
            for (i = 0; i < 5; i++)
            {
                sysop_poke(0xd020, color + i);
            }

            // cycle 15 is where the first char is fetched on a badline
            // but using it is delayed so we also delay setting the color here
            sysop_wait_vic2(waitLine, 17);
            for (i = 0; i < 40; i++)
            {
                sysop_poke(0xd021, color + 5 + i);
            }

            sysop_wait_vic2(waitLine, 57);
            for (i = 0; i < 4; i++)
            {
                sysop_poke(0xd020, color + 14 + i);
            }
            sysop_poke(0xd020, 0);
            sysop_poke(0xd021, 0);

            waitLine += 1;
            count++;

            if (count == 8)
            {
                waitLine++;
                count = 1;
            }
        }
        else
        {
            waitLine += 1;
            sysop_poke(0xd020, 0);
            sysop_poke(0xd021, 0);
        }

        if (waitLine >= g_vic_lines - 1)
        {
            waitLine = 0;
            count = 1;
        }
    }

    return 0;
}
