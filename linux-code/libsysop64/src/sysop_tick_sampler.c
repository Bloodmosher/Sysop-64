/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "sysop_internal.h"

enum {
    TICK_SAMPLER_CFG_CONTROL = 0,
    TICK_SAMPLER_CFG_RANGE = 1,
    TICK_SAMPLER_CFG_SAMPLE_LIMIT = 2,
    TICK_SAMPLER_CFG_BUFFER_START = 3,
    TICK_SAMPLER_CFG_BUFFER_END = 4,
};

enum {
    TICK_SAMPLER_STATUS_CONTROL = 0,
    TICK_SAMPLER_STATUS_WRITE_ADDR = 1,
    TICK_SAMPLER_STATUS_WRAP_COUNT = 2,
    TICK_SAMPLER_STATUS_SAMPLE_COUNT = 3,
    TICK_SAMPLER_STATUS_FIFO_WRITES = 4,
    TICK_SAMPLER_STATUS_SDRAM_WRITES = 5,
    TICK_SAMPLER_STATUS_DROPPED = 6,
};

static void wait_cmd3_complete(void)
{
    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val >> 63) == 1) {
        val = *((uint64_t*)sysop64_cmd3_result_map);
    }
}

static void write_tick_sampler_config(uint8_t index, uint32_t value)
{
    *((uint64_t*)sysop64_cmd3_param_map) = value;
    *((uint32_t*)sysop64_cmd3_map) = (SYSOP64_CMD3_SET_TICK_SAMPLER_CONFIG << 24) | index;
    wait_cmd3_complete();
}

static uint32_t read_tick_sampler_status(uint8_t index)
{
    *((uint32_t*)sysop64_cmd3_map) = (SYSOP64_CMD3_READ_TICK_SAMPLER_STATUS << 24) | index;
    wait_cmd3_complete();
    return (uint32_t)(*((uint64_t*)sysop64_cmd3_result_map) & 0xffffffff);
}

void sysop_tick_sampler_default_config(struct sysop_tick_sampler_config* config)
{
    if (config == NULL) {
        return;
    }

    config->selected_tick = 44;
    config->continuous = true;
    config->writes_only = false;
    config->range_filter = false;
    config->range_start = 0x0000;
    config->range_end = 0xffff;
    config->capture_sample_limit = 0;
    config->buffer_start_address = SYSOP64_TICK_SAMPLER_BUFFER_ADDRESS;
    config->buffer_end_address = SYSOP64_TICK_SAMPLER_BUFFER_ADDRESS + SYSOP64_TICK_SAMPLER_BUFFER_SIZE;
}

void sysop_tick_sampler_configure(const struct sysop_tick_sampler_config* config)
{
    if (config == NULL) {
        return;
    }

    uint32_t control =
        ((uint32_t)config->selected_tick) |
        ((uint32_t)(config->continuous ? 1 : 0) << 8) |
        ((uint32_t)(config->writes_only ? 1 : 0) << 9) |
        ((uint32_t)(config->range_filter ? 1 : 0) << 10);

    get_library_lock();
    write_tick_sampler_config(TICK_SAMPLER_CFG_CONTROL, control);
    write_tick_sampler_config(
        TICK_SAMPLER_CFG_RANGE,
        ((uint32_t)config->range_start << 16) | config->range_end
    );
    write_tick_sampler_config(TICK_SAMPLER_CFG_SAMPLE_LIMIT, config->capture_sample_limit);
    write_tick_sampler_config(TICK_SAMPLER_CFG_BUFFER_START, config->buffer_start_address);
    write_tick_sampler_config(TICK_SAMPLER_CFG_BUFFER_END, config->buffer_end_address);
    release_library_lock();
}

void sysop_tick_sampler_start()
{
    *((uint16_t*)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_TRIGGER_TICK_SAMPLER;
}

void sysop_tick_sampler_stop()
{
    *((uint16_t*)sysop64_cmd_address) = (uint16_t)SYSOP64_CMD_ID_STOP_TICK_SAMPLER;
}

void sysop_tick_sampler_read_status(struct sysop_tick_sampler_status* status)
{
    if (status == NULL) {
        return;
    }

    get_library_lock();
    status->control = read_tick_sampler_status(TICK_SAMPLER_STATUS_CONTROL);
    status->current_write_address = read_tick_sampler_status(TICK_SAMPLER_STATUS_WRITE_ADDR);
    status->wrap_count = read_tick_sampler_status(TICK_SAMPLER_STATUS_WRAP_COUNT);
    status->sample_count = read_tick_sampler_status(TICK_SAMPLER_STATUS_SAMPLE_COUNT);
    status->fifo_write_count = read_tick_sampler_status(TICK_SAMPLER_STATUS_FIFO_WRITES);
    status->sdram_write_count = read_tick_sampler_status(TICK_SAMPLER_STATUS_SDRAM_WRITES);
    status->dropped = read_tick_sampler_status(TICK_SAMPLER_STATUS_DROPPED);
    release_library_lock();
}

void* sysop_tick_sampler_map_buffer(uint32_t size)
{
    return sysop_tick_sampler_map_buffer_at(SYSOP64_TICK_SAMPLER_BUFFER_ADDRESS, size);
}

void* sysop_tick_sampler_map_buffer_at(uint32_t address, uint32_t size)
{
    return mmap(
        NULL,
        size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        sysop_bridge_fd(),
        address
    );
}

uint32_t sysop_tick_sampler_ring_used_bytes(uint32_t read_offset, uint32_t write_addr, uint32_t wrap_count, uint32_t* hps_wrap_count)
{
    if (hps_wrap_count == NULL) {
        return 0;
    }

    uint32_t write_offset = write_addr - SYSOP64_TICK_SAMPLER_BUFFER_ADDRESS;
    uint64_t fpga_total = ((uint64_t)wrap_count * SYSOP64_TICK_SAMPLER_BUFFER_SIZE) + write_offset;
    uint64_t hps_total = ((uint64_t)(*hps_wrap_count) * SYSOP64_TICK_SAMPLER_BUFFER_SIZE) + read_offset;

    if (fpga_total < hps_total) {
        return 0;
    }

    uint64_t used = fpga_total - hps_total;
    if (used > UINT32_MAX) {
        return UINT32_MAX;
    }

    return (uint32_t)used;
}
