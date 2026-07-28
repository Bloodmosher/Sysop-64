#!/usr/bin/env python3
# Sysop-64
# https://github.com/Bloodmosher/Sysop-64
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Sysop-64 Project

"""Decode c64_bus_monitor_debug_sampler captures.

Each record is 32 bytes, little-endian:
  word0: tick-style raw C64 bus sample
  word1: c64_bus_monitor debug_state
  word2: c64_bus_monitor debug_extra
  word3: reserved
"""

import argparse
import struct

RECORD_BYTES = 32


def decode_bus(raw):
    return {
        "data": raw & 0xFF,
        "addr": (raw >> 8) & 0xFFFF,
        "r_w": (raw >> 24) & 1,
        "ba": (raw >> 25) & 1,
        "phi2": (raw >> 26) & 1,
        "loram": (raw >> 27) & 1,
        "hiram": (raw >> 28) & 1,
        "charen": (raw >> 29) & 1,
        "cycle": (raw >> 30) & 0xFF,
        "line": (raw >> 38) & 0x1FF,
        "tick": (raw >> 47) & 0x3F,
        "match": (raw >> 53) & 1,
        "dma": (raw >> 54) & 1,
        "irq": (raw >> 55) & 1,
        "frame": (raw >> 56) & 0xFF,
    }


def decode_state(raw):
    return {
        "synced": (raw >> 63) & 1,
        "cpu_bus_cycle": (raw >> 62) & 1,
        "state": (raw >> 58) & 0xF,
        "sync_state": (raw >> 55) & 0x7,
        "cycle_count": (raw >> 51) & 0xF,
        "cycles_needed": (raw >> 47) & 0xF,
        "bytes_needed": (raw >> 45) & 0x3,
        "opcode_latch": (raw >> 37) & 0xFF,
        "current_pc": (raw >> 21) & 0xFFFF,
        "operand_lo": (raw >> 13) & 0xFF,
        "reg_a_hi": (raw >> 8) & 0x1F,
        "inferred_port_01": raw & 0xFF,
    }


def decode_extra(raw):
    return {
        "irq_seq": (raw >> 60) & 1,
        "branch_taken": (raw >> 59) & 1,
        "port01_write": (raw >> 58) & 1,
        "write_inference": (raw >> 57) & 1,
        "read_capture": (raw >> 56) & 1,
        "current_opcode": (raw >> 48) & 0xFF,
        "reg_a": (raw >> 40) & 0xFF,
        "reg_x": (raw >> 32) & 0xFF,
        "reg_y": (raw >> 24) & 0xFF,
        "operand_hi": (raw >> 16) & 0xFF,
        "last_port01_change": (raw >> 8) & 0xFF,
        "last_port01_source": raw & 0xFF,
    }


def chl(bus):
    return (bus["charen"] << 2) | (bus["hiram"] << 1) | bus["loram"]


def event_text(extra):
    events = []
    if extra["read_capture"]:
        events.append("readcap")
    if extra["write_inference"]:
        events.append("writeinf")
    if extra["port01_write"]:
        events.append("port01")
    if extra["branch_taken"]:
        events.append("branch")
    if extra["irq_seq"]:
        events.append("irqseq")
    return ",".join(events) if events else "-"


def format_record(index, bus_raw, state_raw, extra_raw):
    bus = decode_bus(bus_raw)
    state = decode_state(state_raw)
    extra = decode_extra(extra_raw)
    rw = "R" if bus["r_w"] else "W"
    return (
        f"#{index:8d} fr={bus['frame']:3d} l={bus['line']:3d} c={bus['cycle']:3d} "
        f"t={bus['tick']:2d} {rw} ${bus['addr']:04X}=${bus['data']:02X} "
        f"CHL={chl(bus):X} match={bus['match']} irq={bus['irq']} dma={bus['dma']} "
        f"syn={state['synced']} cpu={state['cpu_bus_cycle']} "
        f"st={state['state']:X} ss={state['sync_state']:X} cc={state['cycle_count']} cn={state['cycles_needed']} bn={state['bytes_needed']} "
        f"op=${state['opcode_latch']:02X} cur=${extra['current_opcode']:02X} pc=${state['current_pc']:04X} "
        f"lo=${state['operand_lo']:02X} hi=${extra['operand_hi']:02X} "
        f"A=${extra['reg_a']:02X} X=${extra['reg_x']:02X} Y=${extra['reg_y']:02X} "
        f"p01=${state['inferred_port_01']:02X} chg=${extra['last_port01_change']:02X} src=${extra['last_port01_source']:02X} "
        f"ev={event_text(extra)}"
    )


def main():
    parser = argparse.ArgumentParser(description="Decode c64_bus_monitor_debug_sampler captures")
    parser.add_argument("capture")
    parser.add_argument("--start", type=int, default=0, help="first record index to print")
    parser.add_argument("--count", type=int, default=200, help="maximum records to print")
    parser.add_argument("--events-only", action="store_true", help="print only records with monitor event flags")
    parser.add_argument("--mismatches-only", action="store_true", help="print only records where inferred $0001 != CHL")
    args = parser.parse_args()

    with open(args.capture, "rb") as f:
        data = f.read()

    total = len(data) // RECORD_BYTES
    printed = 0
    for index in range(args.start, total):
        off = index * RECORD_BYTES
        bus_raw, state_raw, extra_raw, _reserved = struct.unpack_from("<QQQQ", data, off)
        bus = decode_bus(bus_raw)
        extra = decode_extra(extra_raw)
        has_event = any(extra[name] for name in ("irq_seq", "branch_taken", "port01_write", "write_inference", "read_capture"))
        if args.events_only and not has_event:
            continue
        if args.mismatches_only and bus["match"] != 0:
            continue
        print(format_record(index, bus_raw, state_raw, extra_raw))
        printed += 1
        if printed >= args.count:
            break


if __name__ == "__main__":
    main()
