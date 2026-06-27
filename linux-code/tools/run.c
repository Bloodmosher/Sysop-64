/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sysop64.h"

void poke_run()
{
    uint16_t addr = 0x277;
    sysop_poke(addr++, 0x52);
    sysop_poke(addr++, 0x55);
    sysop_poke(addr++, 0x4e);
    sysop_poke(addr++, 0x0d);
    sysop_poke(0x00c6, 0x04);
}

int main(int argc, char **argv)
{
    sysop_init();
    int result = sysop_server_connect();
    if (result != 0)
    {
        printf("sysop_server_connect failed %d\n", result);
        exit(-1);
    }

    sysop_server_dma_lock();
    poke_run();
    sysop_server_dma_unlock();
    // sysop_server_console_close(); should not need now with lock vs lock2
    sysop_server_disconnect();
    sysop_uninit();
    return result;
}
