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
#include <getopt.h>
#include <string.h>
#include "sysop64.h"

int test_shadow_memory()
{
    printf("Testing shadow memory\n");
    int iterations = 10;
    int wrong = 0;
    for (int x=0;x<iterations;x++) {
        int i;
        uint16_t addr = 0x400;
        uint8_t value;
        printf("Shadow memory test iteration %d\n", x);
        for (i=0;i<0x9BFF;i++)
        {
            sysop_poke(addr+i, (uint8_t)i);
            sysop_dma_wait_empty();
            sysop_dma_wait_not_busy();
            value = sysop_internal_peek(addr+i);
            if (value != (uint8_t)i)
            {
                printf("Shadow: sysop_peek error on iteration %d, address %04X, expected %02X but got %02X\n", x, (uint16_t)(addr+i), (uint8_t)i, value);
                wrong++;
            }
        }
    }
    printf("Total wrong values in shadow memory test: %d\n", wrong);
    return wrong;
}


int test_ram_dma()
{
    printf("Testing RAM DMA\n");
    int iterations = 10;
    int wrong = 0;
    for (int x=0;x<iterations;x++) {
        int i;
        uint16_t addr = 0x400;
        uint8_t value;
        printf("RAM DMA test iteration %d\n", x);
        for (i=0;i<0x9BFF;i++)
        {
            sysop_poke(addr+i, (uint8_t)i);
            sysop_dma_wait_empty();
            sysop_dma_wait_not_busy();
            value = sysop_peek(addr+i);
            if (value != (uint8_t)i)
            {
                printf("RAM DMA: sysop_peek error on iteration %d, address %04X, expected %02X but got %02X\n", x, (uint16_t)(addr+i), (uint8_t)i, value);
                wrong++;
            }
        }
    }
    printf("Total wrong values in RAM DMA memory test: %d\n", wrong);
    return wrong;
}

int test_kernal_memory()
{
    printf("Testing kernal memory\n");

    sysop_command(18);
    
    //printf("TODO: using sysop_sampler_start()\n");
    //sysop_sampler_start();

    int iterations = 10;
    int wrong = 0;
    for (int x=0;x<iterations;x++) {
        int i;
        uint16_t addr = 0;
        uint8_t value;
        printf("Kernal memory test iteration %d\n", x);
        for (i=0;i<0x2000;i++)
        {
            sysop_kernal_poke(addr+i, (uint8_t)i);
            sysop_dma_wait_empty();
            sysop_dma_wait_not_busy();
            value = sysop_peek(0xe000+addr+i);
            if (value != (uint8_t)i)
            {
                printf("Kernal: sysop_peek error on iteration %d, address %04X, expected %02X but got %02X\n", x, (uint16_t)(addr+i), (uint8_t)i, value);
                wrong++;
                //break;
            }
        }
    }
    printf("Total wrong values in kernal memory test: %d\n", wrong);
    sysop_command(19);

    return wrong;
}

int test_ultimax_memory()
{
    printf("Testing ultimax memory\n");

    sysop_cartridge_enable_ultimax();
    
    int iterations = 10;
    int wrong = 0;
    int wrongTotal = 0;
    for (int x=0;x<iterations;x++) {
        int i;
        uint16_t addr = 0;
        uint8_t value;
        printf("Ultimax ROML memory test iteration %d\n", x);
        for (i=0;i<0x2000;i++)
        {
            sysop_cartridge_poke(addr+i, (uint8_t)i);
            sysop_dma_wait_empty();
            sysop_dma_wait_not_busy();
            value = sysop_peek(0x8000+addr+i);
            if (value != (uint8_t)i)
            {
                printf("Ultimax: sysop_peek error on iteration %d, address %04X, expected %02X but got %02X\n", x, (uint16_t)(addr+i), (uint8_t)i, value);
                wrong++;
                //break;
            }
        }
    }
    printf("Total wrong values in ultimax memory test (ROML): %d\n", wrong);
    wrongTotal = wrong;
    wrong = 0;
    iterations = 10;
    for (int x=0;x<iterations;x++) {
        int i;
        uint16_t addr = 0x2000;
        uint8_t value;
        printf("Ultimax ROMH memory test iteration %d\n", x);
        for (i=0;i<0x2000;i++)
        {
            sysop_cartridge_poke(addr+i, (uint8_t)i);
            sysop_dma_wait_empty();
            sysop_dma_wait_not_busy();
            value = sysop_peek(0xe000+i);
            if (value != (uint8_t)i)
            {
                printf("Ultimax: sysop_peek error on iteration %d, address %04X, expected %02X but got %02X\n", x, (uint16_t)(addr+i), (uint8_t)i, value);
                wrong++;
                //break;
            }
        }
    }
    printf("Total wrong values in ultimax memory test (ROMH): %d\n", wrong);
    sysop_cartridge_disable();
    wrongTotal += wrong;
    return wrongTotal;
}



int main(int argc, char** argv)
{
    sysop_init();
//printf("TODO: bypassing dma broker\n");

    int result = sysop_server_connect();
    if (result != 0)
    {
        perror("sysop_server_connect() failed\n");
        return -1;
    }


    sysop_enable_io();
    sysop_server_dma_lock();
    //sysop_dma_enable();

    if (argc > 1 && strcmp(argv[1], "shadow")==0)
    {
        test_shadow_memory();
        goto exit;
    }

    if (argc > 1 && strcmp(argv[1], "kernal")==0)
    {
        test_kernal_memory();
        goto exit;
    }

    if (argc > 1 && strcmp(argv[1], "ultimax")==0)
    {
        test_ultimax_memory();
        goto exit;
    }

    if (argc > 1 && strcmp(argv[1], "ram")==0)
    {
        test_ram_dma();
        goto exit;
    }

    printf("Testing IO space\n");
    int iterations = 100; //1000;
    int wrong = 0;
    int wrongTotal = 0;
    for (int x=0;x<iterations;x++) {
        int i;
        uint16_t addr = 0xde00;
        uint8_t value;
        for (i=0;i<512;i++)
        {
            sysop_io_poke(addr+i, (uint8_t)i);
            //sysop_poke(addr+i, (uint8_t)i);
            //sysop_dma_wait_empty();
            //sysop_dma_wait_not_busy();
            value = sysop_io_peek(addr+i);
            //value = sysop_peek(addr+i);
            if (value != (uint8_t)i)
            {
                printf("sysop_io_peek error on iteration %d, expected %d but got %d\n", x, i, value);
                wrong++;
            //    break;
            }
            //sysop_dma_wait_empty();
            //sysop_dma_wait_not_busy();
            value = sysop_peek(addr+i);
            if (value != (uint8_t)i)
            {
                printf("sysop_peek error on iteration %d, expected %d but got %d\n", x, (uint8_t)i, value);
                wrong++;
             //   break;
            }
            //sysop_poke(addr+i, 0);
            //sysop_dma_wait_empty();
            //sysop_dma_wait_not_busy();
            value = sysop_io_peek(addr+i);

            sysop_io_poke(addr+i, 0);
            //sysop_dma_wait_empty();
            //sysop_dma_wait_not_busy();
            value = sysop_peek(addr+i);
            if (value != 0)
            {
                printf("sysop_io_peek error on iteration %d, expected 0 but got %d\n", x, value);
                wrong++;
                break;
            }
        }
    }
    printf("Total wrong values: %d\n", wrong);

    printf("Testing cartridge IO\n");
    sysop_cartridge_enable(0x4000);
    iterations = 100;
    wrongTotal += wrong;
    wrong = 0;
    for (int x=0;x<iterations;x++) {
        int i;
        uint16_t addr = 0x8000;
        uint8_t value;
        for (i=0;i<0x4000;i++)
        {
            sysop_cartridge_poke(addr+i, (uint8_t)i);
            sysop_dma_wait_empty();
            sysop_dma_wait_not_busy();
            value = sysop_peek(addr+i);
            if (value != (uint8_t)i)
            {
                printf("Cartridge: sysop_peek error on iteration %d, address %04X, expected %02X but got %02X\n", x, (uint16_t)(addr+i), (uint8_t)i, value);
                wrong++;
                //break;
            }
        }
    }
    printf("Total wrong values: %d\n", wrong);

    sysop_cartridge_disable();

    wrongTotal += test_shadow_memory();

    wrongTotal += test_kernal_memory();

    wrongTotal += test_ultimax_memory();

    wrongTotal += test_ram_dma();

    if (wrongTotal == 0) {
        printf("Result: PASS\n");
    }
    else { 
        printf("Result: FAIL\n");
    }

exit:
    sysop_server_dma_unlock();
    sysop_server_disconnect();

    sysop_uninit();
}
