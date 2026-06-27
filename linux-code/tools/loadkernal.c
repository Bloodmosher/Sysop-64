/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "sysop64.h"

int main(int argc, char **argv)
{
    sysop_init();
    int result = sysop_server_connect();
    if (result != 0)
    {
        printf("sysop_server_connect failed %d\n", result);
        sysop_uninit();
        return -1;
    }
    sysop_server_dma_lock();

    if (argc >= 3 && argv[2][0] == 'v')
        sysop_kernal_load(argv[1], 1);
    else
        sysop_kernal_load(argv[1], 0);

    sysop_server_dma_unlock();
    sysop_server_disconnect();
    sysop_uninit();
    return 0;
}
