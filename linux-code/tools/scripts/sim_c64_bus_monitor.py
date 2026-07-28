#!/usr/bin/env python3
# Sysop-64
# https://github.com/Bloodmosher/Sysop-64
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Sysop-64 Project

"""Behavioral replay model for rtl/c64/c64_bus_monitor.v.

This is not a general 6510 emulator.  It mirrors the bus-monitor FSM closely
enough to feed tickstream capture files through the same shadow-register logic
and explain how inferred $0001 changes.
"""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path


STATE_FETCH = "FETCH"
STATE_OPERAND_1 = "OPERAND_1"
STATE_OPERAND_2 = "OPERAND_2"
STATE_EXECUTE = "EXECUTE"
STATE_IRQ_SEQ = "IRQ_SEQ"
STATE_BRANCH_CHECK = "BRANCH_CHECK"
STATE_PAGE_CHECK = "PAGE_CHECK"
STATE_BRANCH_RESOLVE = "BRANCH_RESOLVE"
STATE_BRANCH_CROSS_WAIT = "BRANCH_CROSS_WAIT"

SYNC_WAIT_LDA_IMM = "WAIT_LDA_IMM"
SYNC_WAIT_LDA_VALUE = "WAIT_LDA_VALUE"
SYNC_WAIT_STA_ZP = "WAIT_STA_ZP"
SYNC_WAIT_STA_OPERAND = "WAIT_STA_OPERAND"
SYNC_WAIT_STA_01_WRITE = "WAIT_STA_01_WRITE"

KERNAL_PORT01_INIT_PC = 0xFDD5
KERNAL_PORT01_INIT_VALUE = 0xE7


@dataclass(frozen=True)
class Sample:
    index: int
    raw: int
    frame: int
    irq: int
    dma: int
    match: int
    tick: int
    line: int
    cycle: int
    charen: int
    hiram: int
    loram: int
    phi2: int
    ba: int
    rw: int
    addr: int
    data: int

    @property
    def chl(self) -> int:
        return (self.charen << 2) | (self.hiram << 1) | self.loram

    @property
    def op(self) -> str:
        return "R" if self.rw else "W"


@dataclass
class DecodedInstruction:
    bytes_needed: int = 1
    cycles_needed: int = 2
    is_branch: bool = False
    may_page_cross: bool = False


class MonitorModel:
    def __init__(
        self,
        trace_all: bool = False,
        trace_branches: bool = False,
        trace_immediates: bool = False,
    ):
        self.trace_all = trace_all
        self.trace_branches = trace_branches
        self.trace_immediates = trace_immediates
        self.reset()

    def reset(self) -> None:
        self.state = STATE_FETCH
        self.sync_state = SYNC_WAIT_LDA_IMM
        self.monitor_synced = False
        self.cycle_count = 0
        self.reg_a = 0
        self.reg_x = 0
        self.reg_y = 0
        self.reg_i_flag = 0
        self.pending_irq_trigger = False
        self.current_pc = 0
        self.last_write_pc = 0
        self.branch_crosses_page = False
        self.inferred_port_01 = 0x37
        self.current_opcode = 0
        self.opcode_latch = 0
        self.operand_lo = 0
        self.operand_hi = 0
        self.potential_opcode = 0
        self.potential_opcode_addr = 0
        self.bytes_needed = 1
        self.cycles_needed = 2
        self.is_branch = False
        self.may_page_cross = False
        self.expected_next_pc = 0
        self.events: list[str] = []

    def step(self, s: Sample) -> list[str]:
        self.events = []

        # Matches Verilog guard: cycle_start_pulse && dma_s && (ba_s || !rw_s).
        # Tickstream captures only selected sample points and does not include
        # dma_n, so assume no external DMA while replaying this capture.
        if not (s.ba or not s.rw):
            return []

        if not self.monitor_synced:
            self._step_sync(s)
            return self.events

        old = self._snapshot()
        nxt = dict(old)

        def set_next(name: str, value: int | bool | str) -> None:
            nxt[name] = value

        if self.state == STATE_FETCH:
            if not s.rw:
                pass
            if False:
                pass
            if 0 and not self.reg_i_flag:
                # IRQ is not present in the tick capture format.  Keep the
                # branch here documented, but unreachable in this replay model.
                pass
            else:
                decoded = decode_instruction(s.data)
                set_next("opcode_latch", s.data)
                set_next("current_opcode", s.data)
                set_next("current_pc", s.addr)
                set_next("cycle_count", 1)
                self._apply_decode(nxt, decoded)
                if decoded.bytes_needed > 1:
                    set_next("state", STATE_OPERAND_1)
                elif decoded.cycles_needed > 1:
                    set_next("state", STATE_EXECUTE)
                else:
                    set_next("state", STATE_FETCH)
                self._log_fetch(s, old)

        elif self.state == STATE_OPERAND_1:
            set_next("operand_lo", s.data)
            set_next("cycle_count", self.cycle_count + 1)
            self._immediate_update(nxt, self.opcode_latch, s.data, s)

            if self.is_branch:
                set_next("state", STATE_BRANCH_CHECK)
                set_next("branch_crosses_page", is_branch_page_cross(self.current_pc, s.data))
            elif self.bytes_needed > 2:
                set_next("state", STATE_OPERAND_2)
            elif self.cycle_count + 1 < self.cycles_needed:
                set_next("state", STATE_EXECUTE)
            elif self.may_page_cross:
                set_next("state", STATE_PAGE_CHECK)
            else:
                set_next("state", STATE_FETCH)

        elif self.state == STATE_OPERAND_2:
            set_next("operand_hi", s.data)
            set_next("cycle_count", self.cycle_count + 1)
            if self.cycle_count + 1 < self.cycles_needed:
                set_next("state", STATE_EXECUTE)
            elif self.may_page_cross:
                set_next("state", STATE_PAGE_CHECK)
            else:
                set_next("state", STATE_FETCH)

        elif self.state == STATE_EXECUTE:
            set_next("cycle_count", self.cycle_count + 1)
            if s.rw:
                self._perform_read_capture(nxt, s)
            else:
                if self.opcode_latch == 0x48:
                    set_next("reg_a", s.data)
                elif self.opcode_latch == 0x08:
                    set_next("reg_i_flag", (s.data >> 2) & 1)

            if self.cycle_count + 1 == self.cycles_needed:
                if self.may_page_cross:
                    set_next("state", STATE_PAGE_CHECK)
                else:
                    self._update_flags(nxt, self.opcode_latch)
                    if not s.rw:
                        if self.bytes_needed == 2:
                            target = self.operand_lo
                        else:
                            target = (self.operand_hi << 8) | self.operand_lo
                        self._perform_write_inference(nxt, target, s)

                    if self.bytes_needed == 1 and self.cycles_needed == 2:
                        decoded = decode_instruction(s.data)
                        set_next("opcode_latch", s.data)
                        set_next("current_opcode", s.data)
                        set_next("current_pc", s.addr)
                        set_next("cycle_count", 1)
                        self._apply_decode(nxt, decoded)
                        if decoded.bytes_needed > 1:
                            set_next("state", STATE_OPERAND_1)
                        elif decoded.cycles_needed > 1:
                            set_next("state", STATE_EXECUTE)
                        else:
                            set_next("state", STATE_FETCH)
                    else:
                        set_next("state", STATE_FETCH)

        elif self.state == STATE_BRANCH_CHECK:
            set_next("cycle_count", self.cycle_count + 1)
            set_next("potential_opcode", s.data)
            set_next("potential_opcode_addr", s.addr)
            set_next("state", STATE_BRANCH_RESOLVE)
            if self.trace_branches:
                self.events.append(
                    f"branch dummy candidate {fmt_sample(s)} potential=${s.data:02X}"
                )

        elif self.state == STATE_BRANCH_RESOLVE:
            if s.addr == ((self.potential_opcode_addr + 1) & 0xFFFF):
                decoded = decode_instruction(self.potential_opcode)
                set_next("opcode_latch", self.potential_opcode)
                set_next("current_opcode", self.potential_opcode)
                set_next("current_pc", self.potential_opcode_addr)
                self._apply_decode(nxt, decoded)
                set_next("cycle_count", 2)
                set_next("operand_lo", s.data)
                self._immediate_update(nxt, self.potential_opcode, s.data, s)
                if decoded.bytes_needed > 2:
                    set_next("state", STATE_OPERAND_2)
                elif decoded.cycles_needed > 2:
                    set_next("state", STATE_EXECUTE)
                elif decoded.bytes_needed == 1 and decoded.cycles_needed == 2:
                    self._update_flags(nxt, self.potential_opcode)
                    decoded2 = decode_instruction(s.data)
                    set_next("opcode_latch", s.data)
                    set_next("current_opcode", s.data)
                    set_next("current_pc", s.addr)
                    set_next("cycle_count", 1)
                    self._apply_decode(nxt, decoded2)
                    if decoded2.bytes_needed > 1:
                        set_next("state", STATE_OPERAND_1)
                    elif decoded2.cycles_needed > 1:
                        set_next("state", STATE_EXECUTE)
                    else:
                        set_next("state", STATE_FETCH)
                else:
                    set_next("state", STATE_FETCH)
                if self.trace_branches or 0xEA5C <= s.addr <= 0xEA80:
                    self.events.append(
                        f"branch not taken {fmt_sample(s)} commit=${self.potential_opcode:02X}"
                    )
            else:
                if self.trace_branches or 0xEA5C <= s.addr <= 0xEA80:
                    self.events.append(
                        f"branch taken {fmt_sample(s)} discard=${self.potential_opcode:02X} "
                        f"target_opcode=${s.data:02X}"
                    )
                if self.branch_crosses_page:
                    set_next("state", STATE_BRANCH_CROSS_WAIT)
                else:
                    decoded = decode_instruction(s.data)
                    set_next("opcode_latch", s.data)
                    set_next("current_opcode", s.data)
                    set_next("current_pc", s.addr)
                    set_next("cycle_count", 1)
                    self._apply_decode(nxt, decoded)
                    if decoded.bytes_needed > 1:
                        set_next("state", STATE_OPERAND_1)
                    elif decoded.cycles_needed > 1:
                        set_next("state", STATE_EXECUTE)
                    else:
                        set_next("state", STATE_FETCH)

        elif self.state == STATE_BRANCH_CROSS_WAIT:
            decoded = decode_instruction(s.data)
            set_next("opcode_latch", s.data)
            set_next("current_opcode", s.data)
            set_next("current_pc", s.addr)
            set_next("cycle_count", 1)
            self._apply_decode(nxt, decoded)
            if decoded.bytes_needed > 1:
                set_next("state", STATE_OPERAND_1)
            elif decoded.cycles_needed > 1:
                set_next("state", STATE_EXECUTE)
            else:
                set_next("state", STATE_FETCH)

        elif self.state == STATE_PAGE_CHECK:
            expected_next_pc = (self.current_pc + self.bytes_needed) & 0xFFFF
            set_next("expected_next_pc", expected_next_pc)
            if s.addr == expected_next_pc:
                decoded = decode_instruction(s.data)
                set_next("opcode_latch", s.data)
                set_next("current_opcode", s.data)
                set_next("current_pc", s.addr)
                set_next("cycle_count", 1)
                self._apply_decode(nxt, decoded)
                if decoded.bytes_needed > 1:
                    set_next("state", STATE_OPERAND_1)
                elif decoded.cycles_needed > 1:
                    set_next("state", STATE_EXECUTE)
                else:
                    set_next("state", STATE_FETCH)
            else:
                set_next("cycle_count", self.cycle_count + 1)
                if not s.rw and s.addr == 0x0001:
                    set_next("inferred_port_01", self.reg_a)
                    set_next("last_write_pc", self.current_pc)
                    self._log_port01_write(s, old, self.reg_a, "page-check")
                elif s.rw:
                    self._perform_read_capture(nxt, s)
                set_next("state", STATE_FETCH)

        elif self.state == STATE_IRQ_SEQ:
            set_next("cycle_count", self.cycle_count + 1)
            if self.cycle_count == 5:
                set_next("reg_i_flag", 1)
            if not s.rw and s.addr == 0x0001:
                set_next("inferred_port_01", s.data)
                self._log_port01_write(s, old, s.data, "irq-failsafe")
            if s.addr == 0xFFFF and s.rw:
                set_next("state", STATE_FETCH)
                set_next("pending_irq_trigger", False)
                set_next("reg_i_flag", 1)

        self._commit(nxt)
        if self.trace_all:
            self.events.insert(0, self._trace_line(s, old))
        return self.events

    def _step_sync(self, s: Sample) -> None:
        if self.sync_state == SYNC_WAIT_LDA_IMM:
            if s.rw and s.addr == KERNAL_PORT01_INIT_PC and s.data == 0xA9:
                self.current_pc = s.addr
                self.current_opcode = s.data
                self.sync_state = SYNC_WAIT_LDA_VALUE
                self.events.append(f"sync saw LDA # at {fmt_sample(s)}")

        elif self.sync_state == SYNC_WAIT_LDA_VALUE:
            if (
                s.rw
                and s.addr == KERNAL_PORT01_INIT_PC + 1
                and s.data == KERNAL_PORT01_INIT_VALUE
            ):
                self.reg_a = KERNAL_PORT01_INIT_VALUE
                self.sync_state = SYNC_WAIT_STA_ZP
                self.events.append(f"sync seed A=${self.reg_a:02X} at {fmt_sample(s)}")
            elif s.rw and s.addr == KERNAL_PORT01_INIT_PC and s.data == 0xA9:
                self.current_pc = s.addr
                self.current_opcode = s.data
                self.sync_state = SYNC_WAIT_LDA_VALUE
            else:
                self.sync_state = SYNC_WAIT_LDA_IMM

        elif self.sync_state == SYNC_WAIT_STA_ZP:
            if s.rw and s.addr == KERNAL_PORT01_INIT_PC + 2 and s.data == 0x85:
                self.current_pc = s.addr
                self.current_opcode = s.data
                self.opcode_latch = s.data
                self.sync_state = SYNC_WAIT_STA_OPERAND
            elif s.rw and s.addr == KERNAL_PORT01_INIT_PC and s.data == 0xA9:
                self.current_pc = s.addr
                self.current_opcode = s.data
                self.sync_state = SYNC_WAIT_LDA_VALUE
            else:
                self.sync_state = SYNC_WAIT_LDA_IMM

        elif self.sync_state == SYNC_WAIT_STA_OPERAND:
            if s.rw and s.addr == KERNAL_PORT01_INIT_PC + 3 and s.data == 0x01:
                self.operand_lo = s.data
                self.sync_state = SYNC_WAIT_STA_01_WRITE
            elif s.rw and s.addr == KERNAL_PORT01_INIT_PC and s.data == 0xA9:
                self.current_pc = s.addr
                self.current_opcode = s.data
                self.sync_state = SYNC_WAIT_LDA_VALUE
            else:
                self.sync_state = SYNC_WAIT_LDA_IMM

        elif self.sync_state == SYNC_WAIT_STA_01_WRITE:
            if not s.rw and s.addr == 0x0001:
                self.inferred_port_01 = KERNAL_PORT01_INIT_VALUE
                self.last_write_pc = self.current_pc
                self.monitor_synced = True
                self.state = STATE_FETCH
                self.cycle_count = 0
                self.bytes_needed = 1
                self.cycles_needed = 2
                self.is_branch = False
                self.may_page_cross = False
                self.branch_crosses_page = False
                self.pending_irq_trigger = False
                self.sync_state = SYNC_WAIT_LDA_IMM
                self.events.append(f"sync locked inferred=$E7 at {fmt_sample(s)}")
            elif s.rw and s.addr == KERNAL_PORT01_INIT_PC and s.data == 0xA9:
                self.current_pc = s.addr
                self.current_opcode = s.data
                self.sync_state = SYNC_WAIT_LDA_VALUE
            else:
                self.sync_state = SYNC_WAIT_LDA_IMM

    def _snapshot(self) -> dict[str, int | bool | str]:
        return {
            "state": self.state,
            "monitor_synced": self.monitor_synced,
            "cycle_count": self.cycle_count,
            "reg_a": self.reg_a,
            "reg_x": self.reg_x,
            "reg_y": self.reg_y,
            "reg_i_flag": self.reg_i_flag,
            "pending_irq_trigger": self.pending_irq_trigger,
            "current_pc": self.current_pc,
            "last_write_pc": self.last_write_pc,
            "branch_crosses_page": self.branch_crosses_page,
            "inferred_port_01": self.inferred_port_01,
            "current_opcode": self.current_opcode,
            "opcode_latch": self.opcode_latch,
            "operand_lo": self.operand_lo,
            "operand_hi": self.operand_hi,
            "potential_opcode": self.potential_opcode,
            "potential_opcode_addr": self.potential_opcode_addr,
            "bytes_needed": self.bytes_needed,
            "cycles_needed": self.cycles_needed,
            "is_branch": self.is_branch,
            "may_page_cross": self.may_page_cross,
            "expected_next_pc": self.expected_next_pc,
        }

    def _commit(self, values: dict[str, int | bool | str]) -> None:
        for name, value in values.items():
            setattr(self, name, value)
        self.reg_a &= 0xFF
        self.reg_x &= 0xFF
        self.reg_y &= 0xFF
        self.inferred_port_01 &= 0xFF
        self.current_pc &= 0xFFFF
        self.last_write_pc &= 0xFFFF

    def _apply_decode(self, nxt: dict[str, int | bool | str], d: DecodedInstruction) -> None:
        nxt["bytes_needed"] = d.bytes_needed
        nxt["cycles_needed"] = d.cycles_needed
        nxt["is_branch"] = d.is_branch
        nxt["may_page_cross"] = d.may_page_cross

    def _immediate_update(
        self, nxt: dict[str, int | bool | str], opcode: int, data: int, s: Sample
    ) -> None:
        if opcode == 0xA9:
            nxt["reg_a"] = data
        elif opcode == 0xA2:
            nxt["reg_x"] = data
        elif opcode == 0xA0:
            nxt["reg_y"] = data
        elif opcode == 0x29:
            nxt["reg_a"] = self.reg_a & data
        elif opcode == 0x09:
            nxt["reg_a"] = self.reg_a | data
            if self.trace_immediates or 0xEA5C <= self.current_pc <= 0xEA80:
                self.events.append(
                    f"ORA #${data:02X} at {fmt_sample(s)} pc=${self.current_pc:04X} "
                    f"A ${self.reg_a:02X}->${(self.reg_a | data) & 0xFF:02X}"
                )
        elif opcode == 0x49:
            nxt["reg_a"] = self.reg_a ^ data

    def _perform_read_capture(self, nxt: dict[str, int | bool | str], s: Sample) -> None:
        capture_val = self.inferred_port_01 if s.addr == 0x0001 else s.data
        if s.addr == 0x0001:
            self.events.append(
                f"LDA/read $0001 at {fmt_sample(s)} capture=${capture_val:02X} "
                f"A ${self.reg_a:02X}->${capture_val:02X} inferred=${self.inferred_port_01:02X}"
            )

        if self.opcode_latch in (0xA5, 0xB5, 0xAD, 0xBD, 0xB9, 0xA1, 0xB1):
            nxt["reg_a"] = capture_val
        elif self.opcode_latch in (0xA6, 0xB6, 0xAE, 0xBE):
            nxt["reg_x"] = capture_val
        elif self.opcode_latch in (0xA4, 0xB4, 0xAC, 0xBC):
            nxt["reg_y"] = capture_val
        elif self.opcode_latch == 0xAF:
            nxt["reg_a"] = capture_val
            nxt["reg_x"] = capture_val
        elif self.opcode_latch == 0x68:
            nxt["reg_a"] = capture_val
        elif self.opcode_latch in (0x05, 0x15, 0x0D, 0x1D, 0x19, 0x01, 0x11):
            nxt["reg_a"] = self.reg_a | capture_val
        elif self.opcode_latch in (0x25, 0x35, 0x2D, 0x3D, 0x39, 0x21, 0x31):
            nxt["reg_a"] = self.reg_a & capture_val
        elif self.opcode_latch in (0x45, 0x55, 0x4D, 0x5D, 0x59, 0x41, 0x51):
            nxt["reg_a"] = self.reg_a ^ capture_val

    def _perform_write_inference(
        self, nxt: dict[str, int | bool | str], target_addr: int, s: Sample
    ) -> None:
        if target_addr != 0x0001 or s.rw:
            return

        value = self.inferred_port_01
        source = "unchanged"
        if self.opcode_latch in (0x85, 0x8D):
            value = self.reg_a
            source = "A"
        elif self.opcode_latch in (0x86, 0x8E):
            value = self.reg_x
            source = "X"
        elif self.opcode_latch in (0x84, 0x8C):
            value = self.reg_y
            source = "Y"
        elif self.opcode_latch in (0xE6, 0xEE):
            value = self.inferred_port_01 + 1
            source = "INC"
        elif self.opcode_latch in (0xC6, 0xCE):
            value = self.inferred_port_01 - 1
            source = "DEC"
        elif self.opcode_latch in (0x06, 0x0E):
            value = (self.inferred_port_01 << 1) & 0xFF
            source = "ASL"
        elif self.opcode_latch in (0x46, 0x4E):
            value = self.inferred_port_01 >> 1
            source = "LSR"

        nxt["last_write_pc"] = self.current_pc
        nxt["inferred_port_01"] = value & 0xFF
        self._log_port01_write(s, self._snapshot(), value & 0xFF, source)

    def _update_flags(self, nxt: dict[str, int | bool | str], opcode: int) -> None:
        if opcode == 0xAA:
            nxt["reg_x"] = self.reg_a
        elif opcode == 0xA8:
            nxt["reg_y"] = self.reg_a
        elif opcode == 0x8A:
            nxt["reg_a"] = self.reg_x
        elif opcode == 0x98:
            nxt["reg_a"] = self.reg_y
        elif opcode == 0x78:
            nxt["reg_i_flag"] = 1
        elif opcode == 0x58:
            nxt["reg_i_flag"] = 0

    def _log_fetch(self, s: Sample, old: dict[str, int | bool | str]) -> None:
        if s.addr in (0xEA61, 0xEA6B, 0xEA6D, 0xEA6F, 0xEA71, 0xEA79):
            self.events.append(
                f"fetch {fmt_sample(s)} opcode=${s.data:02X} "
                f"A=${old['reg_a']:02X} inferred=${old['inferred_port_01']:02X}"
            )

    def _log_port01_write(
        self, s: Sample, old: dict[str, int | bool | str], value: int, source: str
    ) -> None:
        self.events.append(
            f"WRITE $0001 {fmt_sample(s)} pc=${old['current_pc']:04X} "
            f"op=${old['opcode_latch']:02X} source={source} "
            f"A=${old['reg_a']:02X} X=${old['reg_x']:02X} Y=${old['reg_y']:02X} "
            f"inferred ${old['inferred_port_01']:02X}->${value & 0xFF:02X} "
            f"actual_chl={s.chl:X} match_bit={s.match}"
        )

    def _trace_line(self, s: Sample, old: dict[str, int | bool | str]) -> str:
        return (
            f"{fmt_sample(s)} state={old['state']} pc=${old['current_pc']:04X} "
            f"op=${old['opcode_latch']:02X} cyc={old['cycle_count']} "
            f"A=${old['reg_a']:02X} inferred=${old['inferred_port_01']:02X}"
        )


def decode_instruction(op: int) -> DecodedInstruction:
    d = DecodedInstruction()
    if op in (0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0):
        d.bytes_needed = 2
        d.cycles_needed = 2
        d.is_branch = True
    elif op in (
        0x1D,
        0x3D,
        0x5D,
        0x7D,
        0x9D,
        0xBD,
        0xDD,
        0xFD,
        0x19,
        0x39,
        0x59,
        0x79,
        0x99,
        0xB9,
        0xD9,
        0xF9,
    ):
        d.bytes_needed = 3
        d.cycles_needed = 4
        d.may_page_cross = True
    elif op in (0x11, 0x31, 0x51, 0x71, 0x91, 0xB1, 0xD1, 0xF1):
        d.bytes_needed = 2
        d.cycles_needed = 5
        d.may_page_cross = True
    elif op in (0x01, 0x21, 0x41, 0x61, 0x81, 0xA1, 0xC1, 0xE1):
        d.bytes_needed = 2
        d.cycles_needed = 6
    elif op in (0x09, 0x29, 0x49, 0x69, 0xA9, 0xC9, 0xE9, 0xA2, 0xA0, 0xC0, 0xE0):
        d.bytes_needed = 2
        d.cycles_needed = 2
    elif op in (0x05, 0x25, 0x45, 0x65, 0x85, 0xA5, 0xC5, 0xE5, 0x86, 0xA6, 0x84, 0xA4, 0xC4, 0xE4):
        d.bytes_needed = 2
        d.cycles_needed = 3
    elif op in (0x15, 0x35, 0x55, 0x75, 0x95, 0xB5, 0xD5, 0xF5, 0x96, 0xB6, 0x94, 0xB4):
        d.bytes_needed = 2
        d.cycles_needed = 4
    elif op in (0x0D, 0x2D, 0x4D, 0x6D, 0x8D, 0xAD, 0xCD, 0xED, 0x8E, 0xAE, 0x8C, 0xAC, 0xCC, 0xEC, 0xAF):
        d.bytes_needed = 3
        d.cycles_needed = 4
    elif op in (0x06, 0x26, 0x46, 0x66, 0xC6, 0xE6):
        d.bytes_needed = 2
        d.cycles_needed = 5
    elif op in (0x16, 0x36, 0x56, 0x76, 0xD6, 0xF6):
        d.bytes_needed = 2
        d.cycles_needed = 6
    elif op in (0x0E, 0x2E, 0x4E, 0x6E, 0xCE, 0xEE):
        d.bytes_needed = 3
        d.cycles_needed = 6
    elif op in (0x1E, 0x3E, 0x5E, 0x7E, 0xDE, 0xFE):
        d.bytes_needed = 3
        d.cycles_needed = 7
    elif op == 0x4C:
        d.bytes_needed = 3
        d.cycles_needed = 3
    elif op == 0x6C:
        d.bytes_needed = 3
        d.cycles_needed = 5
    elif op == 0x20:
        d.bytes_needed = 3
        d.cycles_needed = 6
    elif op in (0x60, 0x40):
        d.bytes_needed = 1
        d.cycles_needed = 6
    elif op == 0x00:
        d.bytes_needed = 1
        d.cycles_needed = 7
    elif op in (0x48, 0x08):
        d.bytes_needed = 1
        d.cycles_needed = 3
    elif op in (0x68, 0x28):
        d.bytes_needed = 1
        d.cycles_needed = 4
    elif op in (0xAA, 0xA8, 0x8A, 0x98, 0x9A, 0xBA, 0x18, 0x38, 0x58, 0x78, 0xB8, 0xD8, 0xF8, 0xEA):
        d.bytes_needed = 1
        d.cycles_needed = 2
    return d


def is_branch_page_cross(pc: int, offset: int) -> bool:
    next_pc = (pc + 2) & 0xFFFF
    signed_offset = offset - 0x100 if offset & 0x80 else offset
    target = (next_pc + signed_offset) & 0xFFFF
    return (next_pc & 0xFF00) != (target & 0xFF00)


def parse_sample(index: int, raw: int) -> Sample:
    return Sample(
        index=index,
        raw=raw,
        frame=(raw >> 56) & 0xFF,
        irq=(raw >> 55) & 0x01,
        dma=(raw >> 54) & 0x01,
        match=(raw >> 53) & 0x01,
        tick=(raw >> 47) & 0x3F,
        line=(raw >> 38) & 0x1FF,
        cycle=(raw >> 30) & 0xFF,
        charen=(raw >> 29) & 0x01,
        hiram=(raw >> 28) & 0x01,
        loram=(raw >> 27) & 0x01,
        phi2=(raw >> 26) & 0x01,
        ba=(raw >> 25) & 0x01,
        rw=(raw >> 24) & 0x01,
        addr=(raw >> 8) & 0xFFFF,
        data=raw & 0xFF,
    )


def iter_samples(path: Path):
    with path.open("rb") as f:
        index = 0
        while chunk := f.read(8):
            if len(chunk) != 8:
                raise ValueError(f"trailing partial sample of {len(chunk)} bytes")
            (raw,) = struct.unpack("<Q", chunk)
            yield parse_sample(index, raw)
            index += 1


def fmt_sample(s: Sample) -> str:
    return (
        f"#{s.index} fr={s.frame:3d} t={s.tick:02d} "
        f"l={s.line:3d} c={s.cycle:2d} {s.op} ${s.addr:04X}=${s.data:02X}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Replay tickstream samples through a Python mirror of c64_bus_monitor.v"
    )
    parser.add_argument("capture", type=Path, help="tickstream capture file")
    parser.add_argument("--max-samples", type=int, default=0, help="stop after N samples")
    parser.add_argument("--start", type=int, default=0, help="first sample index to process")
    parser.add_argument(
        "--stop-on-mismatch",
        action="store_true",
        help="stop when simulated inferred $01[2:0] differs from captured CHL",
    )
    parser.add_argument(
        "--stop-on-capture-mismatch",
        action="store_true",
        help="stop when the capture match bit says FPGA inferred $01 no longer matches CHL",
    )
    parser.add_argument(
        "--trace-all",
        action="store_true",
        help="print every processed sample with monitor state",
    )
    parser.add_argument(
        "--trace-branches",
        action="store_true",
        help="print branch dummy/resolve events outside the EA6B debug path",
    )
    parser.add_argument(
        "--trace-immediates",
        action="store_true",
        help="print all immediate ORA/AND/EOR-style debug events, not just the EA6B path",
    )
    parser.add_argument(
        "--print-start",
        type=int,
        default=0,
        help="suppress event printing before this sample index while still simulating",
    )
    parser.add_argument(
        "--print-end",
        type=int,
        default=0,
        help="suppress event printing after this sample index while still simulating",
    )
    parser.add_argument(
        "--around-ea6b",
        action="store_true",
        help="print extra lines around the KERNAL LDA $01 / ORA #$20 / STA $01 path",
    )
    args = parser.parse_args()

    model = MonitorModel(
        trace_all=args.trace_all,
        trace_branches=args.trace_branches,
        trace_immediates=args.trace_immediates,
    )
    processed = 0
    first_mismatch = None
    first_capture_mismatch = None

    for s in iter_samples(args.capture):
        if s.index < args.start:
            continue
        if args.max_samples and processed >= args.max_samples:
            break

        events = model.step(s)
        processed += 1

        in_print_window = (
            s.index >= args.print_start
            and (args.print_end == 0 or s.index <= args.print_end)
        )
        should_print = bool(events) and in_print_window
        if args.around_ea6b and 0xEA5C <= s.addr <= 0xEA80:
            should_print = in_print_window

        simulated_match = (model.inferred_port_01 & 0x07) == s.chl
        if model.monitor_synced and not simulated_match and first_mismatch is None:
            first_mismatch = s
            should_print = in_print_window
            events.append(
                f"SIM MISMATCH after sample: inferred=${model.inferred_port_01:02X} "
                f"inferred_chl={model.inferred_port_01 & 7:X} actual_chl={s.chl:X} "
                f"capture_match_bit={s.match}"
            )

        if model.monitor_synced and s.match == 0 and first_capture_mismatch is None:
            first_capture_mismatch = s
            if args.stop_on_capture_mismatch:
                should_print = True
            events.append(
                f"CAPTURE MISMATCH bit fell at {fmt_sample(s)} "
                f"model_inferred=${model.inferred_port_01:02X} "
                f"model_chl={model.inferred_port_01 & 7:X} actual_chl={s.chl:X} "
                f"state={model.state} pc=${model.current_pc:04X} op=${model.opcode_latch:02X} A=${model.reg_a:02X}"
            )

        if should_print:
            if args.around_ea6b and 0xEA5C <= s.addr <= 0xEA80 and not events:
                events = [
                    f"{fmt_sample(s)} state={model.state} pc=${model.current_pc:04X} "
                    f"op=${model.opcode_latch:02X} A=${model.reg_a:02X} "
                    f"inferred=${model.inferred_port_01:02X}"
                ]
            for event in events:
                print(event)

        if args.stop_on_mismatch and first_mismatch is not None:
            break
        if args.stop_on_capture_mismatch and first_capture_mismatch is not None:
            break

    print(
        f"processed={processed} synced={int(model.monitor_synced)} "
        f"inferred=${model.inferred_port_01:02X} A=${model.reg_a:02X} "
        f"state={model.state} pc=${model.current_pc:04X}"
    )
    if first_mismatch:
        print(f"first_sim_mismatch={fmt_sample(first_mismatch)}")
    if first_capture_mismatch:
        print(f"first_capture_mismatch={fmt_sample(first_capture_mismatch)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
