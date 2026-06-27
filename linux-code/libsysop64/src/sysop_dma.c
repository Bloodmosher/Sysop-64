/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "sysop_internal.h"

static int waitcount = 0;
static int busyWaitCount = 0;

void sysop_dma_wait_not_busy()
{
    uint64_t status = *((uint64_t *)sysop64_fpga_status_map);
    while ((status & 0x10000000000000) != 0) {
        status = *((uint64_t *)sysop64_fpga_status_map);
    }
}

void sysop_dma_wait_empty()
{
    uint64_t status = *((uint64_t *)sysop64_fpga_status_map);
    while ((status & 0x8000000000000000)>>63 != 1) {
        status = *((uint64_t *)sysop64_fpga_status_map);
    }
}

void dma_wait_not_full()
{
    uint64_t status = *((uint64_t *)sysop64_fpga_status_map);
    while ((status & 0x4000000000000000)>>62 == 1) {
        status = *((uint64_t *)sysop64_fpga_status_map);
    }
}

uint16_t sysop_dma_write_queue_length()
{
    uint64_t data = *((uint64_t *)sysop64_fpga_status_map);
    uint16_t len = (uint16_t)((data & 0x3FF800000000)>>35);
    return len;
}

void sysop_dma_write_tag(uint32_t tag)
{
    dma_wait_not_full();
    uint32_t val = ((uint32_t)67<<24) | (tag & 0x00FFFFFF);
    *((uint32_t*)sysop64_cmd2_map) = val;
}

void sysop_poke_no_wait(uint16_t address, uint8_t value)
{
    uint32_t val = ((uint32_t)1<<24) | (((uint32_t)address)<<8) | value;
    *((uint32_t*)sysop64_cmd2_map) = val;
}

uint32_t sysop_dma_tag_data()
{
    uint64_t data = *((uint64_t*)sysop64_dma_tag_data_map);
    return (uint32_t)(data & 0x00FFFFFF);
}

void sysop_dma_queue_freeze()
{
    dma_wait_not_full();

    uint32_t val = ((uint32_t)16<<24);
    *((uint32_t*)sysop64_cmd2_map) = val;
}

void sysop_dma_queue_unfreeze()
{
    dma_wait_not_full();
    
    uint32_t val = ((uint32_t)32<<24);
    *((uint32_t*)sysop64_cmd2_map) = val;
}

void sysop_dma_freeze()
{
    *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_DMA_FREEZE;
}

void sysop_dma_unfreeze()
{
    *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_DMA_UNFREEZE;
}

void sysop_dma_enable()
{
    sysop_dma_queue_freeze();
    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();
}

void sysop_dma_disable()
{
    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();
    sysop_dma_queue_unfreeze();
    sysop_dma_wait_empty();
    sysop_dma_wait_not_busy();
}
