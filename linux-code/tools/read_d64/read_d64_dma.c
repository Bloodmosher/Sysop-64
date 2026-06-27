/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "read_d64_private.h"

static void log_dma_wait_context(const char *label, uint32_t dmainfo, uint32_t spin)
{
    uint8_t cmd_requested = sysop_io_peek(CMD_REQUESTED);
    uint8_t cmd_status = sysop_io_peek(CMD_STATUS);
    uint8_t io_status = sysop_io_peek(IO_STATUS_RETURN);
    uint8_t df01 = sysop_internal_peek(0xDF01);

    printf("[read_d64 dma] %s spin=%u dmainfo=%08X CMD_REQUESTED=%02X CMD_STATUS=%02X IO_STATUS=%02X DF01=%02X\n",
           label,
           spin,
           dmainfo,
           cmd_requested,
           cmd_status,
           io_status,
           df01);
}

void dump_info()
{
    uint8_t cpu_a = sysop_io_peek(CPU_A);
    uint8_t cpu_x = sysop_io_peek(CPU_X);
    uint8_t cpu_y = sysop_io_peek(CPU_Y);
    //uint8_t mem_ba = sysop_io_peek(0x00ba);
    //uint8_t mem_90 = sysop_io_peek(0x0090);
    //dbg_printf("CPU_A $%02X CPU_X $%02X CPU_Y $%02X $00BA $%02X $0090 $%02X\n", cpu_a, cpu_x, cpu_y, mem_ba, mem_90);
    dbg_printf("CPU_A $%02X CPU_X $%02X CPU_Y $%02X\n", cpu_a, cpu_x, cpu_y);
}


int requeue_count = 0;

void dma_enable_with_verify()
{
    uint32_t dmainfo = sysop_get_dma_info();
    printf("enable dmainfo entry: %08X\n", dmainfo);
    sysop_dma_enable();
    uint32_t spin = 0;
    while (1) {
        dmainfo = sysop_get_dma_info();
        printf("enable dmainfo: %08X\n", dmainfo);
        if ((dmainfo & 0x80000000) != 0) {
            usleep(10000);
            //printf("no dma, sleeping\n");
            spin++;
            if ((spin % 100) == 0)
                printf("enable: no dma, sleeping\n");

            if ((spin % 1000) == 0)
            {
                printf("was dma enable dropped? requeuing...\n");
                requeue_count++;
                sysop_dma_enable();
            }
            //    sysop_command(1);

            continue;
        }
        else break;
    }
}

void dma_disable_with_verify()
{
    const uint32_t pre_disable_timeout_spins = 200;
    const uint32_t post_disable_timeout_spins = 300;
    uint32_t dmainfo = sysop_get_dma_info();
    printf("disable dmainfo entry: %08X\n", dmainfo);
    uint32_t spin = 0;
    while(1) {
        dmainfo = sysop_get_dma_info();
        if ((spin % 25) == 0)
            printf("disable dmainfo: %08X\n", dmainfo);
        if ((dmainfo & 0x80000000) != 0) {
            usleep(10000);
            //printf("no dma, sleeping\n");
            spin++;
            if ((spin % 100) == 0) {
                printf("disable part 1: no dma, sleeping\n");
                log_dma_wait_context("pre-disable wait", dmainfo, spin);
            }
            if (spin >= pre_disable_timeout_spins) {
                log_dma_wait_context("pre-disable timeout, forcing sysop_dma_disable", dmainfo, spin);
                break;
            }
            continue;
        }
        else break;
    }

    sysop_dma_disable();
    //sysop_command(2);

    spin = 0;
    while(1) {
        dmainfo = sysop_get_dma_info();
        if ((spin % 25) == 0)
            printf("disable (2) dmainfo: %08X\n", dmainfo);
        if ((dmainfo & 0x80000000) == 0) {
            usleep(10000);
            //printf("no dma, sleeping\n");
            spin++;
            if ((spin % 100) == 0) {
                printf("still have dma, sleeping\n");
                log_dma_wait_context("post-disable wait", dmainfo, spin);
            }
            if (spin >= post_disable_timeout_spins) {
                log_dma_wait_context("post-disable timeout, continuing", dmainfo, spin);
                break;
            }
            continue;
        }
        else break;
    }

    //usleep(100000);
}

void dma_enable_wrapper()
{
    dma_enable_with_verify();
}

void dma_disable_wrapper()
{
    dma_disable_with_verify();
}
