/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "sysop_internal.h"

void sysop_sampler_wait_not_busy()
{
    get_library_lock();
    uint64_t status = *((uint64_t *)sysop64_fpga_status_map);
    while ((status & 0x80000000000000) != 0) {
        status = *((uint64_t *)sysop64_fpga_status_map);
    }
   release_library_lock();
}

void sysop_sampler_start()
{
    *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_TRIGGER_SAMPLER;
}

static uint8_t* g_sampler_buffer = NULL;

uint64_t sysop_sampler_get_sample(uint32_t index, struct sysop_c64_bus_sample* pSample)
{
    if (g_sampler_buffer == NULL)
    {
        g_sampler_buffer = (uint8_t*)mmap(NULL, SYSOP64_SAMPLER_BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, sysop_bridge_fd(), SYSOP64_SAMPLER_BUFFER_ADDRESS);
    }
    if (g_sampler_buffer != NULL) 
    {
        uint64_t* pData = (uint64_t*)g_sampler_buffer;
        uint32_t i = index;
        if (pSample != NULL)
        {
            pSample->raw = pData[i];
            pSample->data = pData[i] & 0xFF;
            pSample->addr = (pData[i] & 0x0ffff00)>>8;
            pSample->r__w = ((pData[i] & (1<<24))>>24)&1;
            pSample->ba = (pData[i] & ((uint64_t)1<<25))>>25;
            pSample->phi2 = (pData[i] & ((uint64_t)1<<26))>>26;
            pSample->_irq = (pData[i] & ((uint64_t)1<<27))>>27;
            pSample->_dma = (pData[i] & ((uint64_t)1<<28))>>28;
            pSample->freeze_signal = (pData[i] & ((uint64_t)1<<29))>>29;
            pSample->cycle = (pData[i] & 0x3FC0000000)>>30;
            pSample->vic_line = (pData[i] & 0x7FC000000000)>>38;
            pSample->sample_tick = (pData[i] & 0x1F800000000000)>>47;
            //pSample->phi2_counter_val = (pData[i] & 0xFFE0000000000000)>>53;
            pSample->_roml = (pData[i] & 0x2000000000000000)>>61;
            pSample->_romh = (pData[i] & 0x1000000000000000)>>60;
            pSample->_io1 = (pData[i] & 0x800000000000000)>>59;
            pSample->_io2 = (pData[i] & 0x400000000000000)>>58;
            pSample->_charen = (pData[i] & 0x200000000000000)>>57;
            pSample->_hiram = (pData[i] & 0x100000000000000)>>56;
            pSample->_loram = (pData[i] & 0x80000000000000)>>55;
            pSample->_exrom = (pData[i] & 0x40000000000000)>>54;
            pSample->_game = (pData[i] & 0x20000000000000)>>53;
        }

        return pData[i];
    }
    else 
    {
        printf("Failed to map sample buffer address\n");
    }
    return 0;
}
