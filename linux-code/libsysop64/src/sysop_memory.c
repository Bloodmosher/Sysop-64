/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "sysop_internal.h"

void sysop_internal_poke(uint16_t address, uint8_t value);

static void assert_dma_enabled()
{
    uint32_t dmainfo = sysop_get_dma_info();
    if ((dmainfo & 0x80000000) != 0) { // high bit should be a zero
        printf("assert_dma_enabled failed (%08X), hit enter\n", dmainfo);
        getchar();
    }
}

void sysop_poke(uint16_t address, uint8_t value)
{
    dma_wait_not_full();
    
    uint32_t val = ((uint32_t)1<<24) | (((uint32_t)address)<<8) | value;
    *((uint32_t*)sysop64_cmd2_map) = val;
}

void sysop_cartridge_poke(uint16_t address, uint8_t value)
{
    dma_wait_not_full();
    
    // 64 goes through the dma manager, whereas 255 does not
    uint32_t val = ((uint32_t)64<<24) | (((uint32_t)address)<<8) | value;
    
    *((uint32_t*)sysop64_cmd2_map) = val;
}

void sysop_kernal_poke(uint16_t address, uint8_t value)
{
    dma_wait_not_full();
    
    // 65 is kernal sysop_poke in the fpga's dma manager
    uint32_t val = ((uint32_t)65<<24) | (((uint32_t)address)<<8) | value;
    
    *((uint32_t*)sysop64_cmd2_map) = val;
}

uint8_t sysop_peek(uint16_t address)
{
    get_library_lock();
    uint8_t value = 0;

    // since a read is not atomic we need to make sure something did not crash
    // and leave the previous byte waiting to be read...
    uint64_t status = *((uint64_t *)sysop64_fpga_status_map);
    if ((status & 0x2000000000000000)>>61 != 0) {
        value = *((uint8_t *)sysop64_dma_data_map);
    }

    *((uint16_t *)sysop64_dma_address_map) = (uint16_t)address;

    // wait for read request to signal we can read the data
    status = *((uint64_t *)sysop64_fpga_status_map);
    while ((status & 0x2000000000000000)>>61 == 0) {
        status = *((uint64_t *)sysop64_fpga_status_map);
    }

    // issuing the read is what will clear the DMA state machine
    value = *((uint8_t *)sysop64_dma_data_map);

    release_library_lock();
    return value;
}

uint8_t sysop_io_peek(uint16_t address)
{
    return sysop_internal_peek(address);
}

void sysop_io_poke(uint16_t address, uint8_t value)
{
    sysop_internal_poke(address, value);
}

uint8_t sysop_internal_peek(uint16_t address)
{
    get_library_lock();
    *((uint16_t *)sysop64_internal_read_address_map) = (uint16_t)address;
    uint8_t value = *((uint8_t *)sysop64_internal_read_data_map);
    release_library_lock();
    return value;
}

void sysop_internal_poke(uint16_t address, uint8_t value)
{
    get_library_lock();
    uint32_t cmdval =  (SYSOP64_CMD3_ID_IO_POKE <<24) | (address << 8) | value;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val>>63 ) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
        printf("Waiting for cmd3 result 0x%016" PRIx64 "...\n", val);
    }

    release_library_lock();
    // no result in val for this command
}

void sysop_interrupt_test(uint8_t value, int setOrClear)
{
    get_library_lock();
    uint32_t cmdval =  ((setOrClear == 1 ? SYSOP64_CMD3_IRQ_SET : SYSOP64_CMD3_IRQ_CLEAR) <<24) | value;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val>>63 ) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
        printf("Waiting for cmd3 result 0x%016" PRIx64 "...\n", val);
    }
    release_library_lock();

    // no result in val for this command
}


void sysop_add_write_strobe(uint8_t index, uint16_t address)
{
    get_library_lock();
    uint32_t cmdval =  (SYSOP64_CMD3_SETUP_WRITE_STROBE <<24) | (address << 8) | index;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val>>63 ) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
        printf("Waiting for cmd3 result 0x%016" PRIx64 "...\n", val);
    }
    release_library_lock();

    // no result in val for this command
}

void sysop_add_write_io_strobe(uint8_t index, uint16_t address)
{
    get_library_lock();
    uint32_t cmdval =  (SYSOP64_CMD3_SETUP_WRITE_IO_STROBE <<24) | (address << 8) | index;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val>>63 ) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
        printf("Waiting for cmd3 result 0x%016" PRIx64 "...\n", val);
    }
    release_library_lock();

    // no result in val for this command
}

void sysop_add_read_strobe(uint8_t index, uint16_t address)
{
    get_library_lock();
    uint32_t cmdval =  (SYSOP64_CMD3_SETUP_READ_STROBE <<24) | (address << 8) | index;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val>>63 ) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
        printf("Waiting for cmd3 result 0x%016" PRIx64 "...\n", val);
    }
    release_library_lock();

    // no result in val for this command
}

void sysop_add_raster_strobe(uint8_t index, uint16_t line, uint8_t cycle)
{
    get_library_lock();
    uint32_t cmdval = (SYSOP64_CMD3_SETUP_RASTER_STROBE <<24) | ((cycle & 0x7F) << 17) | ((line & 0x1FF) << 8) | index;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val>>63 ) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
        printf("Waiting for cmd3 result 0x%016" PRIx64 "...\n", val);
    }
    release_library_lock();

    // no result in val for this command
}

void sysop_reset_strobe(uint8_t index)
{
    get_library_lock();
    uint32_t cmdval = (SYSOP64_CMD3_RESET_STROBE <<24) | index;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val>>63 ) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
        printf("Waiting for cmd3 result 0x%016" PRIx64 "...\n", val);
    }
    release_library_lock();

    // no result in val for this command
}

void sysop_c64_reset()
{
  *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_RESET;
}

void sysop_enable_io()
{
  *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_ENABLE_IO;
}

void sysop_disable_io()
{
  *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_DISABLE_IO;
}


void sysop_cartridge_enable(uint16_t rom_size)
{
  *((uint16_t *)sysop64_cmd_address) = rom_size == 0x2000 ? (uint16_t)SYSOP64_CMD_ID_ENABLE_CARTRIDGE_8K : (uint16_t)SYSOP64_CMD_ID_ENABLE_CARTRIDGE_16K;
}

void sysop_cartridge_enable_ultimax()
{
  *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_ENABLE_CARTRIDGE_ULTIMAX;
}

void sysop_cartridge_disable()
{
  *((uint16_t *)sysop64_cmd_address) = SYSOP64_CMD_ID_DISABLE_CARTRIDGE;
}

#define SYSOP64_CMD_ID_ENABLE_EASYFLASH_DMA_TRIGGER 26

void sysop_enable_easyflash_dma_trigger()
{
  *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_ENABLE_EASYFLASH_DMA_TRIGGER;
}

void sysop_disable_easyflash_dma_trigger()
{
  *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_DISABLE_EASYFLASH_DMA_TRIGGER;
}

void sysop_enable_reu_dma_trigger()
{
  *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_ENABLE_REU_DMA_TRIGGER;
}

void sysop_disable_reu_dma_trigger()
{
  *((uint16_t *)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_DISABLE_REU_DMA_TRIGGER;
}