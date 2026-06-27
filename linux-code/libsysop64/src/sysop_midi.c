/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include "sysop_internal.h"

uint8_t sysop_namesoft_midi_ready()
{
    get_library_lock();

    uint32_t cmdval =  (SYSOP64_CMD3_NAMESOFT_MIDI_READ_READY<<24);
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val>>63 ) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
    }

    release_library_lock();
    return (uint8_t)(val & 0xFF);
}

void sysop_namesoft_midi_write(uint8_t data)
{
    get_library_lock();
    uint32_t cmdval =  (SYSOP64_CMD3_NAMESOFT_MIDI_WRITE_DATA <<24) | data;
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

void sysop_set_nmi_vector(uint16_t addr)
{
    get_library_lock();
    uint32_t cmdval =  (SYSOP64_CMD3_SET_NMI_VECTOR<<24) | addr;
    *((uint32_t*)sysop64_cmd3_map) = cmdval;

    volatile uint64_t val = *((uint64_t*)sysop64_cmd3_result_map);
    while ((val>>63 ) == 1)
    {
        val = *((uint64_t*)sysop64_cmd3_result_map);
    }
    release_library_lock();

    // no result in val for this command
}
