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

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("loadcart <path to .crt> [-v]\n");
        return 0;
    }
    sysop_init();
    int result = sysop_server_connect();
    if (result != 0)
    {
        printf("sysop_server_connect failed %d\n", result);
        exit(0);
    }
    sysop_server_dma_lock2();

    if (argc >= 3 && strcmp(argv[2], "-v") == 0)
    {
        sysop_cartridge_load(argv[1], 1);
    }
    else // will reset the machine and disable dma
        sysop_cartridge_load(argv[1], 0);

    sysop_c64_reset();
    sysop_server_dma_unlock();
    sysop_server_disconnect();
    sysop_uninit();
    return 0;
}
