/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "sysop_internal.h"

uint32_t sysop_read_inferred_port_01_info()
{
    get_library_lock();

    uint32_t cmdval = SYSOP64_CMD3_READ_INFERRED_PORT_01 << 24;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val >> 63) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
    }

    release_library_lock();

    return (uint32_t)(val & 0xFFFFFFFF);
}

uint8_t sysop_read_inferred_port_01()
{
    return (uint8_t)(sysop_read_inferred_port_01_info() & 0xFF);
}

void sysop_reset_c64_bus_monitor()
{
    get_library_lock();
    uint32_t cmdval = SYSOP64_CMD3_RESET_C64_BUS_MONITOR << 24;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val >> 63) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
    }

    release_library_lock();
}


static uint32_t sysop_cmd3_read_u32(uint8_t cmd)
{
    get_library_lock();

    uint32_t cmdval = ((uint32_t)cmd) << 24;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val >> 63) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
    }

    release_library_lock();

    return (uint32_t)(val & 0xFFFFFFFF);
}

uint32_t sysop_read_port01_debug0()
{
    return sysop_cmd3_read_u32(SYSOP64_CMD3_READ_PORT01_DEBUG0);
}

uint32_t sysop_read_port01_debug1()
{
    return sysop_cmd3_read_u32(SYSOP64_CMD3_READ_PORT01_DEBUG1);
}

uint32_t sysop_read_port01_debug2()
{
    return sysop_cmd3_read_u32(SYSOP64_CMD3_READ_PORT01_DEBUG2);
}

uint32_t sysop_read_port01_debug3()
{
    return sysop_cmd3_read_u32(SYSOP64_CMD3_READ_PORT01_DEBUG3);
}

uint32_t sysop_read_port01_debug4()
{
    return sysop_cmd3_read_u32(SYSOP64_CMD3_READ_PORT01_DEBUG4);
}

uint32_t sysop_read_port01_debug5()
{
    return sysop_cmd3_read_u32(SYSOP64_CMD3_READ_PORT01_DEBUG5);
}

uint32_t sysop_read_port01_debug6()
{
    return sysop_cmd3_read_u32(SYSOP64_CMD3_READ_PORT01_DEBUG6);
}

uint32_t sysop_read_port01_debug7()
{
    return sysop_cmd3_read_u32(SYSOP64_CMD3_READ_PORT01_DEBUG7);
}

uint32_t sysop_read_port01_debug8()
{
    return sysop_cmd3_read_u32(SYSOP64_CMD3_READ_PORT01_DEBUG8);
}

uint32_t sysop_read_port01_debug9()
{
    return sysop_cmd3_read_u32(SYSOP64_CMD3_READ_PORT01_DEBUG9);
}

uint32_t sysop_read_port01_debug10()
{
    return sysop_cmd3_read_u32(SYSOP64_CMD3_READ_PORT01_DEBUG10);
}

uint32_t sysop_read_port01_debug11()
{
    return sysop_cmd3_read_u32(SYSOP64_CMD3_READ_PORT01_DEBUG11);
}

uint32_t sysop_read_port01_debug12()
{
    return sysop_cmd3_read_u32(SYSOP64_CMD3_READ_PORT01_DEBUG12);
}

uint32_t sysop_read_port01_debug13()
{
    return sysop_cmd3_read_u32(SYSOP64_CMD3_READ_PORT01_DEBUG13);
}

uint32_t sysop_read_port01_debug14()
{
    return sysop_cmd3_read_u32(SYSOP64_CMD3_READ_PORT01_DEBUG14);
}

uint32_t sysop_read_port01_debug15()
{
    return sysop_cmd3_read_u32(SYSOP64_CMD3_READ_PORT01_DEBUG15);
}

uint32_t sysop_read_port01_debug16()
{
    return sysop_cmd3_read_u32(SYSOP64_CMD3_READ_PORT01_DEBUG16);
}

uint32_t sysop_read_port01_debug17()
{
    return sysop_cmd3_read_u32(SYSOP64_CMD3_READ_PORT01_DEBUG17);
}