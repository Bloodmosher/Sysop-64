/*
 * Sysop-64
 * https://github.com/Bloodmosher/Sysop-64
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Sysop-64 Project
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sysop64.h"

static const char* port01_source_name(uint8_t source)
{
    switch (source) {
        case 0x01: return "sync_init";
        case 0x11: return "store_a";
        case 0x12: return "store_x";
        case 0x13: return "store_y";
        case 0x14: return "inc";
        case 0x15: return "dec";
        case 0x16: return "asl";
        case 0x17: return "lsr";
        case 0x21: return "page";
        case 0x22: return "irq";
        case 0xff: return "unknown";
        default: return "none";
    }
}
static const char* a_source_name(uint8_t source)
{
    switch (source) {
        case 0x01: return "sync_init";
        case 0x10: return "imm_lda";
        case 0x11: return "imm_and";
        case 0x12: return "imm_ora";
        case 0x13: return "imm_eor";
        case 0x20: return "read_lda";
        case 0x21: return "read_ora";
        case 0x22: return "read_and";
        case 0x23: return "read_eor";
        case 0x24: return "read_lax";
        case 0x25: return "read_pla";
        case 0x30: return "txa";
        case 0x31: return "tya";
        case 0x40: return "pha_bus";
        default: return "none";
    }
}

static void usage(const char* argv0)
{
    fprintf(stderr,
        "usage: %s [--reset-monitor]\n"
        "\n"
        "options:\n"
        "  --reset-monitor  reset c64_bus_monitor before reading $0001 status\n",
        argv0);
}

int main(int argc, char** argv)
{
    int reset_monitor = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--reset-monitor") == 0) {
            reset_monitor = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (sysop_init() < 0) {
        return 1;
    }

    if (reset_monitor) {
        sysop_reset_c64_bus_monitor();
    }

    uint32_t info = sysop_read_inferred_port_01_info();
    uint8_t value = (uint8_t)(info & 0xFF);
    uint8_t chl_match = (uint8_t)((info >> 10) & 1);
    uint8_t monitor_synced = (uint8_t)((info >> 11) & 1);
    uint8_t actual_chl = (uint8_t)((info >> 12) & 0x7);
    uint32_t port01_debug0 = sysop_read_port01_debug0();
    uint32_t port01_debug1 = sysop_read_port01_debug1();
    uint32_t port01_debug2 = sysop_read_port01_debug2();
    uint32_t port01_debug3 = sysop_read_port01_debug3();
    uint32_t port01_debug4 = sysop_read_port01_debug4();
    uint32_t port01_debug5 = sysop_read_port01_debug5();
    uint32_t port01_debug6 = sysop_read_port01_debug6();
    uint32_t port01_debug7 = sysop_read_port01_debug7();
    uint32_t port01_debug8 = sysop_read_port01_debug8();
    uint32_t port01_debug9 = sysop_read_port01_debug9();
    uint32_t port01_debug10 = sysop_read_port01_debug10();
    uint32_t port01_debug11 = sysop_read_port01_debug11();
    uint32_t port01_debug12 = sysop_read_port01_debug12();
    uint32_t port01_debug13 = sysop_read_port01_debug13();
    uint32_t port01_debug14 = sysop_read_port01_debug14();
    uint32_t port01_debug15 = sysop_read_port01_debug15();
    uint32_t port01_debug16 = sysop_read_port01_debug16();
    uint32_t port01_debug17 = sysop_read_port01_debug17();
    uint8_t inferred_chl = (uint8_t)(value & 0x7);

    printf("inferred_port_01=$%02X inferred_chl=$%X actual_chl=$%X chl_match=%u monitor_synced=%u raw=0x%08X\n",
        value,
        inferred_chl,
        actual_chl,
        chl_match,
        monitor_synced,
        info);

    uint8_t last_value = (uint8_t)(port01_debug0 >> 24);
    uint8_t last_opcode = (uint8_t)(port01_debug0 >> 16);
    uint8_t last_source = (uint8_t)(port01_debug0 >> 8);
    uint8_t last_a = (uint8_t)port01_debug0;
    uint16_t last_target = (uint16_t)(port01_debug1 >> 16);
    uint16_t last_bus = (uint16_t)port01_debug1;
    uint8_t last_x = (uint8_t)(port01_debug2 >> 24);
    uint8_t last_y = (uint8_t)(port01_debug2 >> 16);
    uint8_t current_opcode = (uint8_t)(port01_debug2 >> 8);
    uint8_t current_inferred = (uint8_t)port01_debug2;
    uint8_t last_a_value = (uint8_t)(port01_debug3 >> 24);
    uint8_t last_a_opcode = (uint8_t)(port01_debug3 >> 16);
    uint8_t last_a_source = (uint8_t)(port01_debug3 >> 8);
    uint8_t last_a_previous = (uint8_t)port01_debug3;
    uint16_t last_a_pc = (uint16_t)(port01_debug4 >> 16);
    uint16_t last_a_bus_addr = (uint16_t)port01_debug4;
    uint8_t last_a_bus_data = (uint8_t)(port01_debug5 >> 24);
    uint8_t last_a_capture = (uint8_t)(port01_debug5 >> 16);
    uint8_t port01_a_value = (uint8_t)(port01_debug6 >> 24);
    uint8_t port01_a_opcode = (uint8_t)(port01_debug6 >> 16);
    uint8_t port01_a_source = (uint8_t)(port01_debug6 >> 8);
    uint8_t port01_a_previous = (uint8_t)port01_debug6;
    uint16_t port01_a_pc = (uint16_t)(port01_debug7 >> 16);
    uint16_t port01_a_bus_addr = (uint16_t)port01_debug7;
    uint8_t port01_a_bus_data = (uint8_t)(port01_debug8 >> 24);
    uint8_t port01_a_capture = (uint8_t)(port01_debug8 >> 16);
    uint8_t port01_prev_a_value = (uint8_t)(port01_debug9 >> 24);
    uint8_t port01_prev_a_opcode = (uint8_t)(port01_debug9 >> 16);
    uint8_t port01_prev_a_source = (uint8_t)(port01_debug9 >> 8);
    uint8_t port01_prev_a_previous = (uint8_t)port01_debug9;
    uint16_t port01_prev_a_pc = (uint16_t)(port01_debug10 >> 16);
    uint16_t port01_prev_a_bus_addr = (uint16_t)port01_debug10;
    uint8_t port01_prev_a_bus_data = (uint8_t)(port01_debug11 >> 24);
    uint8_t port01_prev_a_capture = (uint8_t)(port01_debug11 >> 16);
    uint8_t prev_port01_value = (uint8_t)(port01_debug12 >> 24);
    uint8_t prev_port01_opcode = (uint8_t)(port01_debug12 >> 16);
    uint8_t prev_port01_source = (uint8_t)(port01_debug12 >> 8);
    uint8_t prev_port01_a = (uint8_t)port01_debug12;
    uint16_t prev_port01_pc = (uint16_t)(port01_debug13 >> 16);
    uint16_t prev_port01_target = (uint16_t)port01_debug13;
    uint16_t prev_port01_bus = (uint16_t)(port01_debug14 >> 16);
    uint8_t prev_port01_x = (uint8_t)(port01_debug14 >> 8);
    uint8_t prev_port01_y = (uint8_t)port01_debug14;
    uint8_t change_port01_old = (uint8_t)(port01_debug15 >> 24);
    uint8_t change_port01_value = (uint8_t)(port01_debug15 >> 16);
    uint8_t change_port01_opcode = (uint8_t)(port01_debug15 >> 8);
    uint8_t change_port01_source = (uint8_t)port01_debug15;
    uint16_t change_port01_pc = (uint16_t)(port01_debug16 >> 16);
    uint16_t change_port01_target = (uint16_t)port01_debug16;
    uint16_t change_port01_bus = (uint16_t)(port01_debug17 >> 16);
    uint8_t change_port01_a = (uint8_t)(port01_debug17 >> 8);
    uint8_t change_port01_x = (uint8_t)port01_debug17;

    printf("last_port01 value=$%02X opcode=$%02X source=%s($%02X) A=$%02X X=$%02X Y=$%02X target=$%04X bus=$%04X current_opcode=$%02X current_inferred=$%02X raw0=0x%08X raw1=0x%08X raw2=0x%08X\n",
        last_value,
        last_opcode,
        port01_source_name(last_source),
        last_source,
        last_a,
        last_x,
        last_y,
        last_target,
        last_bus,
        current_opcode,
        current_inferred,
        port01_debug0,
        port01_debug1,
        port01_debug2);

    printf("last_A value=$%02X previous=$%02X opcode=$%02X source=%s($%02X) pc=$%04X bus=$%04X bus_data=$%02X capture=$%02X raw3=0x%08X raw4=0x%08X raw5=0x%08X\n",
        last_a_value,
        last_a_previous,
        last_a_opcode,
        a_source_name(last_a_source),
        last_a_source,
        last_a_pc,
        last_a_bus_addr,
        last_a_bus_data,
        last_a_capture,
        port01_debug3,
        port01_debug4,
        port01_debug5);

    printf("port01_A_source value=$%02X previous=$%02X opcode=$%02X source=%s($%02X) pc=$%04X bus=$%04X bus_data=$%02X capture=$%02X raw6=0x%08X raw7=0x%08X raw8=0x%08X\n",
        port01_a_value,
        port01_a_previous,
        port01_a_opcode,
        a_source_name(port01_a_source),
        port01_a_source,
        port01_a_pc,
        port01_a_bus_addr,
        port01_a_bus_data,
        port01_a_capture,
        port01_debug6,
        port01_debug7,
        port01_debug8);

    printf("port01_prev_A_source value=$%02X previous=$%02X opcode=$%02X source=%s($%02X) pc=$%04X bus=$%04X bus_data=$%02X capture=$%02X raw9=0x%08X raw10=0x%08X raw11=0x%08X\n",
        port01_prev_a_value,
        port01_prev_a_previous,
        port01_prev_a_opcode,
        a_source_name(port01_prev_a_source),
        port01_prev_a_source,
        port01_prev_a_pc,
        port01_prev_a_bus_addr,
        port01_prev_a_bus_data,
        port01_prev_a_capture,
        port01_debug9,
        port01_debug10,
        port01_debug11);

    printf("prev_port01 value=$%02X opcode=$%02X source=%s($%02X) A=$%02X X=$%02X Y=$%02X pc=$%04X target=$%04X bus=$%04X raw12=0x%08X raw13=0x%08X raw14=0x%08X\n",
        prev_port01_value,
        prev_port01_opcode,
        port01_source_name(prev_port01_source),
        prev_port01_source,
        prev_port01_a,
        prev_port01_x,
        prev_port01_y,
        prev_port01_pc,
        prev_port01_target,
        prev_port01_bus,
        port01_debug12,
        port01_debug13,
        port01_debug14);

    printf("port01_change old=$%02X value=$%02X opcode=$%02X source=%s($%02X) A=$%02X X=$%02X pc=$%04X target=$%04X bus=$%04X raw15=0x%08X raw16=0x%08X raw17=0x%08X\n",
        change_port01_old,
        change_port01_value,
        change_port01_opcode,
        port01_source_name(change_port01_source),
        change_port01_source,
        change_port01_a,
        change_port01_x,
        change_port01_pc,
        change_port01_target,
        change_port01_bus,
        port01_debug15,
        port01_debug16,
        port01_debug17);

    sysop_uninit();
    return 0;
}
