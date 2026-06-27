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
#include "sysop_defines.h"
#include "sysop64.h"

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        printf("%s <filename> <address-hex>\n", argv[0]);
        return 1;
    }

    printf("Loading %s\n", argv[1]);
    char *filename = argv[1];
    uint16_t address = (uint16_t)strtoll(argv[2], NULL, 16);

    sysop_init();
    sysop_dma_enable();

    sysop_loadbin(filename, address);

    sysop_dma_disable();
    sysop_uninit();

    return 0;
}
