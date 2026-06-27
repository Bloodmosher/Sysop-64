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
#include "sysop_defines.h"

int main(int argc, char **argv)
{
    sysop_open_bridge();
    int result = sysop_server_connect();
    if (result != 0)
    {
        printf("Sysop_Connect failed %d\n", result);
        exit(0);
    }
    /*sysop_dma_enable();
    sysop_load(argv[1]);
    sysop_dma_disable();
    sysop_close_bridge();
  */
    sysop_server_dma_lock2();
    sysop_load(argv[1]);
    sysop_server_dma_unlock();
    sysop_server_disconnect();
    sysop_uninit();
    return 0;
}
