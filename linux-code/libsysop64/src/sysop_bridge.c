/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "sysop_internal.h"

static int fd = 0;

int sysop_init()
{
    return sysop_open_bridge();
}

void sysop_uninit()
{
    sysop_close_bridge();
}

int sysop_open_bridge()
{
    fd = open("/dev/mem", O_RDWR | O_SYNC);

    if (fd < 0) {
        perror("Couldn't open /dev/mem\n");
        return -2;
    }

    sysop64_bridge_map = (uint8_t *)mmap(NULL, SYSOP64_BRIDGE_SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, SYSOP64_BRIDGE);

    if (sysop64_bridge_map == MAP_FAILED) {
        perror("mmap failed.");
        close(fd);
        return -3;
    }

    sysop64_hdmi_cmd_data_map = sysop64_bridge_map + SYSOP64_HDMI_CMD_DATA;
    sysop64_cmd3_param_map = sysop64_bridge_map + SYSOP64_CMD3_PARAM;
    sysop64_dma_address_map = sysop64_bridge_map + SYSOP64_DMA_READ_ADDRESS;
    sysop64_cmd_address = sysop64_bridge_map + SYSOP64_CMD_ADDRESS;
    sysop64_dma_data_map = sysop64_bridge_map + SYSOP64_DMA_READ_DATA;
    sysop64_fpga_status_map = sysop64_bridge_map + SYSOP64_FPGA_STATUS;
    sysop64_poke_dma_address_map = sysop64_bridge_map + SYSOP64_DMA_WRITE_ADDRESS;
    sysop64_poke_dma_data_map = sysop64_bridge_map + SYSOP64_DMA_WRITE_DATA;
    sysop64_internal_read_address_map = sysop64_bridge_map + SYSOP64_INTERNAL_READ_ADDRESS;
    sysop64_internal_read_data_map = sysop64_bridge_map + SYSOP64_INTERNAL_READ_DATA;
    sysop64_c64_signals_map = sysop64_bridge_map + SYSOP64_C64_SIGNALS;
    sysop64_cmd2_map = sysop64_bridge_map + SYSOP64_CMD2_ADDRESS;
    sysop64_phi2_counter_map = sysop64_bridge_map + SYSOP64_PHI2_COUNTER;
    sysop64_debug_data1_map = sysop64_bridge_map + SYSOP64_MAP_DEBUG_DATA1;
    sysop64_cmd3_result_map = sysop64_bridge_map + SYSOP64_CMD3_RESULT;
    sysop64_hdmi_info_result_map = sysop64_bridge_map + SYSOP64_HDMI_INFO_RESULT;
    sysop64_sid_voices_data_map = sysop64_bridge_map + SYSOP64_SID_VOICES_DATA;
    sysop64_gpio_data_map = sysop64_bridge_map + SYSOP64_GPIO_DATA;
    sysop64_dma_read_key_data_map = sysop64_bridge_map + SYSOP64_DMA_READ_KEY_DATA;
    sysop64_dma_tag_data_map = sysop64_bridge_map + SYSOP64_DMA_TAG_DATA;
    sysop64_dma_joystick_data_map = sysop64_bridge_map + SYSOP64_DMA_JOYSTICK_DATA;
    sysop64_audio_status_map = sysop64_bridge_map + SYSOP64_AUDIO_STATUS;
    sysop64_audio_command_map = sysop64_bridge_map + SYSOP64_AUDIO_COMMAND;
    sysop64_set_palette_map = sysop64_bridge_map + SYSOP64_SET_PALETTE_ADDRESS;
    sysop64_cmd3_map = sysop64_bridge_map + SYSOP64_CMD3_ADDRESS;

    return 0;
}

uint64_t sysop_debug1()
{
    return *((volatile uint64_t*)sysop64_debug_data1_map);
}

uint64_t sysop_debug2()
{
   get_library_lock();
   uint64_t val = *((uint64_t*)sysop64_fpga_status_map);
   release_library_lock();
   return val;
}

int sysop_close_bridge()
{
    int result = munmap(sysop64_bridge_map, SYSOP64_BRIDGE_SPAN);

    if (result < 0) {
        perror("Couldnt unmap bridge.");
        close(fd);
        return -4;
    }

    close(fd);
    return 0;
}


uint64_t sysop_read_c64_signals()
{
    return *((uint64_t*)sysop64_c64_signals_map);
}

uint64_t sysop_phi2_counter()
{
    return *((uint64_t*)sysop64_phi2_counter_map);
}

void sysop_wait_hdmi_vblank()
{
    volatile uint64_t val = *((volatile uint64_t*)sysop64_debug_data1_map);
    while ((val>>63 ) == 0)
    {
        val = *((volatile uint64_t*)sysop64_debug_data1_map);
        //printf("Waiting for vblank 0x%016" PRIx64 "...\n", val);
    }
}

int sysop_bridge_fd(void)
{
    return fd;
}
