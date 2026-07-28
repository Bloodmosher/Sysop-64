/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "sysop_internal.h"

static uint8_t sysop_sid_index(uint8_t sid)
{
    return sid == 2 ? 2 : 1;
}

void sysop_sid_filter_table_write_sid(uint8_t sid, uint16_t index, int16_t value)
{
    get_library_lock();

    *((uint64_t *)sysop64_cmd3_param_map) = (uint16_t)value;

    uint8_t sid_index = sysop_sid_index(sid);
    uint32_t command = sid_index == 2 ? SYSOP64_CMD3_WRITE_SID_FILTER_TABLE_SID2 : SYSOP64_CMD3_WRITE_SID_FILTER_TABLE;
    uint32_t cmdval = (command << 24) | (index & 0x3FF);
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val >> 63) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
    }

    release_library_lock();
}

void sysop_sid_filter_table_write(uint16_t index, int16_t value)
{
    sysop_sid_filter_table_write_sid(1, index, value);
}

void sysop_sid_filter_use_custom_sid(uint8_t sid, bool enable)
{
    get_library_lock();

    uint8_t sid_index = sysop_sid_index(sid);
    uint32_t command = sid_index == 2 ? SYSOP64_CMD3_SET_SID_FILTER_CONTROL_SID2 : SYSOP64_CMD3_SET_SID_FILTER_CONTROL;
    uint32_t cmdval = (command << 24) | (enable ? 1u : 0u);
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val >> 63) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
    }

    release_library_lock();
}

void sysop_sid_filter_use_custom(bool enable)
{
    sysop_sid_filter_use_custom_sid(1, enable);
}

static void sysop_sid_filter_set_scale_sid(uint8_t sid, uint8_t model, uint16_t scale_q8_8)
{
    get_library_lock();

    uint8_t sid_index = sysop_sid_index(sid);
    uint32_t command = sid_index == 2 ? SYSOP64_CMD3_SET_SID_FILTER_SCALE_SID2 : SYSOP64_CMD3_SET_SID_FILTER_SCALE;
    uint32_t cmdval = (command << 24) | ((uint32_t)model << 16) | scale_q8_8;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val >> 63) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
    }

    release_library_lock();
}

void sysop_sid_filter_set_scale_6581_sid(uint8_t sid, uint16_t scale_q8_8)
{
    sysop_sid_filter_set_scale_sid(sid, 0, scale_q8_8);
}

void sysop_sid_filter_set_scale_8580_sid(uint8_t sid, uint16_t scale_q8_8)
{
    sysop_sid_filter_set_scale_sid(sid, 1, scale_q8_8);
}

void sysop_sid_filter_set_scale_6581(uint16_t scale_q8_8)
{
    sysop_sid_filter_set_scale_6581_sid(1, scale_q8_8);
}

void sysop_sid_filter_set_scale_8580(uint16_t scale_q8_8)
{
    sysop_sid_filter_set_scale_8580_sid(1, scale_q8_8);
}

void sysop_sid2_enable(bool enable)
{
    sysop_command(enable ? SYSOP64_CMD_ID_ENABLE_SID2 : SYSOP64_CMD_ID_DISABLE_SID2);
}

void sysop_sid2_set_base(uint16_t base_addr)
{
    get_library_lock();

    uint32_t cmdval = (SYSOP64_CMD3_SET_SID2_BASE << 24) | (base_addr & 0xFFE0);
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val >> 63) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
    }

    release_library_lock();
}

void sysop_sid_set_model(uint8_t sid, bool model_8580)
{
    uint8_t sid_index = sysop_sid_index(sid);
    if (sid_index == 2) {
        sysop_command(model_8580 ? SYSOP64_CMD_ID_SELECT_SID2_8580 : SYSOP64_CMD_ID_SELECT_SID2_6581);
    } else {
        sysop_command(model_8580 ? 55 : 54);
    }
}
