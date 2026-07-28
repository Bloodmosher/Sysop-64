#!/usr/bin/env python3
# Sysop-64
# https://github.com/Bloodmosher/Sysop-64
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Sysop-64 Project

"""
Convert one MIDI channel from midicsv output into SID-Wizard pattern/order data.

This is intentionally a first-pass bridge: it reads the readable CSV emitted by
midicsv, keeps Note_on/Note_off events for one MIDI channel, quantizes them to
SID-Wizard pattern rows, and emits either shell commands or direct sidwiz calls. By default it uses a
16-row-per-quarter tracker grid, which keeps short arpeggio-style MIDI
figures from collapsing into a few very dense SID-Wizard rows.

SID-Wizard packed pattern rows used here:
  00          empty/rest row
  7E          GATEOFFX / note off
  80|note ins note-on row with an instrument byte following
  FF          pattern terminator

The note mapping follows the existing sidplaydma/compare tooling convention:
standard MIDI C-5 (60) maps to SID-Wizard note byte 49, so sidwiz_note=midi-11.
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

SIDWIZARD_GATE_OFF = 0x7E
SIDWIZARD_ORDER_NOP = 0xF0
SIDWIZARD_PATTERN_END = 0xFF
SIDWIZARD_BIGFX_TEMPO = 0x10
DEFAULT_ROWS_PER_QUARTER = 16
DEFAULT_DRUM_PLAY_NOTE = 60
MAX_SIDWIZARD_INSTRUMENT = 62
MAX_SIDWIZARD_PATTERN = 100
MAX_SIDWIZARD_PATTERN_BYTES = 249
MAX_SIDWIZARD_ROWS_PER_PATTERN = 249
PAL_FRAMES_PER_SECOND = 50.0
NTSC_FRAMES_PER_SECOND = 60.0

GM_DRUM_NAMES = {
    35: "ACOKICK",
    36: "KICK",
    37: "SIDESTIK",
    38: "SNARE",
    39: "CLAP",
    40: "ELSNARE",
    41: "LOWTOM",
    42: "CLHHAT",
    43: "HITOM",
    44: "PEDHHAT",
    45: "MIDTOM",
    46: "OPHHAT",
    47: "LOWMIDTM",
    48: "HIMIDTOM",
    49: "CRASH1",
    50: "HIGHTOM",
    51: "RIDE",
    52: "CHINA",
    53: "RIDEBELL",
    54: "TAMBRINE",
    55: "SPLASH",
    56: "COWBELL",
    57: "CRASH2",
    58: "VIBSLAP",
    59: "RIDE2",
    60: "HIBONGO",
    61: "LOBONGO",
    62: "MUCONGA",
    63: "OPCONGA",
    64: "LOCONGA",
    65: "HITIMBAL",
    66: "LOTIMBAL",
    67: "HIAGOGO",
    68: "LOAGOGO",
    69: "CABASA",
    70: "MARACAS",
    71: "WHISTLE",
    72: "WHISTLE2",
    73: "GUIRO",
    74: "GUIRO2",
    75: "CLAVES",
    76: "HIWOOD",
    77: "LOWOOD",
    78: "MUTCUI",
    79: "OPCUI",
    80: "MUTTRI",
    81: "OPTRI",
}


@dataclass(frozen=True)
class MidiNoteEvent:
    tick: int
    kind: str
    channel: int
    note: int
    velocity: int
    track: int | None


@dataclass(frozen=True)
class MidiTempoEvent:
    tick: int
    microseconds_per_quarter: int
    track: int | None


def parse_int(text: str) -> int:
    return int(text.strip(), 0)


def parse_midicsv(path: Path, channel: int, track: int | None) -> tuple[int | None, list[MidiNoteEvent], list[MidiTempoEvent]]:
    division: int | None = None
    events: list[MidiNoteEvent] = []
    tempo_events: list[MidiTempoEvent] = []

    with path.open(newline="") as f:
        reader = csv.reader(f, skipinitialspace=True)
        for row in reader:
            if len(row) < 3:
                continue
            try:
                row_track = int(row[0])
                tick = int(row[1])
            except ValueError:
                continue

            event_type = row[2]
            if event_type == "Header" and len(row) >= 6:
                division = parse_int(row[5])
                continue

            if event_type == "Tempo" and len(row) >= 4:
                tempo_events.append(MidiTempoEvent(tick, parse_int(row[3]), row_track))
                continue

            if event_type not in ("Note_on_c", "Note_off_c") or len(row) < 6:
                continue
            if track is not None and row_track != track:
                continue

            event_channel = parse_int(row[3])
            if event_channel != channel:
                continue

            note = parse_int(row[4])
            velocity = parse_int(row[5])
            kind = "off" if event_type == "Note_off_c" or velocity == 0 else "on"
            events.append(MidiNoteEvent(tick, kind, event_channel, note, velocity, row_track))

    events.sort(key=lambda e: (e.tick, 0 if e.kind == "off" else 1))
    tempo_events.sort(key=lambda e: e.tick)
    return division, events, tempo_events


def sidwizard_note_byte(midi_note: int, note_offset: int) -> int:
    return midi_note + note_offset


def drum_instrument_name(midi_note: int) -> str:
    base = GM_DRUM_NAMES.get(midi_note, f"DRUM{midi_note:02d}")
    return base[:8].ljust(8)


def collect_drum_instruments(events: list[MidiNoteEvent], first_instrument: int) -> dict[int, int]:
    notes = sorted({event.note for event in events if event.kind == "on"})
    return {note: first_instrument + index for index, note in enumerate(notes)}


def build_drum_rows(
    events: list[MidiNoteEvent],
    ticks_per_row: float,
    drum_instruments: dict[int, int],
    note_offset: int,
    min_note_rows: int,
    preserve_initial_silence: bool,
    drum_play_note: int,
    preserve_drum_pitch: bool,
) -> tuple[list[list[int]], int]:
    if not events:
        return [], 0

    start_tick = 0 if preserve_initial_silence else min(e.tick for e in events)
    active: dict[int, int] = {}
    rows: dict[int, list[int]] = {}
    collisions = 0

    def set_row(row: int, data: list[int]) -> None:
        nonlocal collisions
        if row < 0:
            return
        if row in rows and rows[row] != data:
            collisions += 1
        rows[row] = data

    for event in events:
        row = row_for_tick(event.tick, start_tick, ticks_per_row)
        if event.kind == "on":
            source_note = event.note if preserve_drum_pitch else drum_play_note
            note_byte = sidwizard_note_byte(source_note, note_offset)
            instrument = drum_instruments.get(event.note)
            if instrument is None or not 1 <= note_byte <= 0x7D:
                continue
            set_row(row, [0x80 | note_byte, instrument])
            active[event.note] = row
        else:
            on_row = active.pop(event.note, None)
            if on_row is None:
                continue
            off_row = max(row, on_row + min_note_rows)
            if rows.get(off_row, [0])[0] & 0x80:
                off_row += 1
            set_row(off_row, [SIDWIZARD_GATE_OFF])

    max_row = max(rows) if rows else -1
    result = [[0x00] for _ in range(max_row + 1)]
    for row, data in rows.items():
        result[row] = data
    return result, collisions


def row_for_tick(tick: int, start_tick: int, ticks_per_row: float) -> int:
    return int(round((tick - start_tick) / ticks_per_row))


def start_tick_for_events(events: list[MidiNoteEvent], preserve_initial_silence: bool) -> int:
    return 0 if preserve_initial_silence else min(e.tick for e in events)


def sidwizard_tempo_value(
    microseconds_per_quarter: int,
    rows_per_quarter: float,
    frames_per_second: float,
    framespeed: int,
    minimum_tempo: int,
) -> int:
    seconds_per_row = (microseconds_per_quarter / 1_000_000.0) / rows_per_quarter
    tempo = int(round(seconds_per_row * frames_per_second * framespeed))
    return max(minimum_tempo, min(0x7F, tempo))


def tempo_events_for_rows(
    tempo_events: list[MidiTempoEvent],
    start_tick: int,
    ticks_per_row: float,
    rows_per_quarter: float,
    frames_per_second: float,
    framespeed: int,
    minimum_tempo: int,
) -> dict[int, int]:
    row_tempos: dict[int, int] = {}
    last_before_start: MidiTempoEvent | None = None

    for event in tempo_events:
        if event.tick < start_tick:
            last_before_start = event
            continue

        row = row_for_tick(event.tick, start_tick, ticks_per_row)
        row_tempos[max(0, row)] = sidwizard_tempo_value(
            event.microseconds_per_quarter,
            rows_per_quarter,
            frames_per_second,
            framespeed,
            minimum_tempo,
        )

    if last_before_start is not None and 0 not in row_tempos:
        row_tempos[0] = sidwizard_tempo_value(
            last_before_start.microseconds_per_quarter,
            rows_per_quarter,
            frames_per_second,
            framespeed,
            minimum_tempo,
        )

    return row_tempos


def tempo_fx_row(tempo: int) -> list[int]:
    """Return SID-Wizard's canonical standalone main-tempo BigFX row.

    SWMconvert documents this exact shape as the tempo-pattern template:
    note-column NOP with next-column bit, instrument-column NOP with
    next-column bit, BigFX $10, then the tempo parameter.
    """
    return [0x80, 0x80, SIDWIZARD_BIGFX_TEMPO, tempo]


def apply_tempo_events(rows: list[list[int]], row_tempos: dict[int, int]) -> list[list[int]]:
    if not row_tempos:
        return rows

    result: list[list[int]] = []
    tempo_items = sorted(row_tempos.items())
    tempo_index = 0
    max_row = max(len(rows), max(row_tempos) + 1)

    for row in range(max_row):
        while tempo_index < len(tempo_items) and tempo_items[tempo_index][0] == row:
            result.append(tempo_fx_row(tempo_items[tempo_index][1]))
            tempo_index += 1
        result.append(rows[row] if row < len(rows) else [0x00])

    while tempo_index < len(tempo_items):
        result.append(tempo_fx_row(tempo_items[tempo_index][1]))
        tempo_index += 1

    return result


def build_rows(
    events: list[MidiNoteEvent],
    ticks_per_row: float,
    instrument: int,
    note_offset: int,
    min_note_rows: int,
    preserve_initial_silence: bool,
) -> list[list[int]]:
    if not events:
        return []

    start_tick = start_tick_for_events(events, preserve_initial_silence)
    active: dict[int, int] = {}
    rows: dict[int, list[int]] = {}

    def set_row(row: int, data: list[int]) -> None:
        if row >= 0:
            rows[row] = data

    for event in events:
        row = row_for_tick(event.tick, start_tick, ticks_per_row)
        if event.kind == "on":
            note_byte = sidwizard_note_byte(event.note, note_offset)
            if not 1 <= note_byte <= 0x7D:
                continue
            set_row(row, [0x80 | note_byte, instrument])
            active[event.note] = row
        else:
            on_row = active.pop(event.note, None)
            if on_row is None:
                off_row = row
            else:
                off_row = max(row, on_row + min_note_rows)
            # A note-on on the same quantized row is more useful than an immediate
            # note-off, especially for short MIDI/percussion-style events.
            if rows.get(off_row, [0])[0] & 0x80:
                off_row += 1
            set_row(off_row, [SIDWIZARD_GATE_OFF])

    max_row = max(rows) if rows else -1
    result = [[0x00] for _ in range(max_row + 1)]
    for row, data in rows.items():
        result[row] = data
    return result


def split_patterns(rows: list[list[int]], rows_per_pattern: int, max_pattern_bytes: int) -> list[list[int]]:
    if rows_per_pattern <= 0:
        raise ValueError("rows_per_pattern must be positive")
    if max_pattern_bytes < 2:
        raise ValueError("max_pattern_bytes must leave room for data and $FF")
    if not rows:
        return [[SIDWIZARD_PATTERN_END]]

    patterns: list[list[int]] = []
    packed: list[int] = []
    rows_in_pattern = 0
    note_is_live = False

    def is_note_on(row: list[int]) -> bool:
        return (
            len(row) >= 2
            and 1 <= (row[0] & 0x7F) <= 0x5F
            and 1 <= (row[1] & 0x7F) <= 0x3E
        )

    def is_gate_off(row: list[int] | None) -> bool:
        return bool(row) and (row[0] & 0x7F) == SIDWIZARD_GATE_OFF

    def finish_pattern() -> None:
        nonlocal packed, rows_in_pattern, note_is_live
        if note_is_live:
            packed.append(SIDWIZARD_GATE_OFF)
        packed.append(SIDWIZARD_PATTERN_END)
        patterns.append(packed)
        packed = []
        rows_in_pattern = 0
        note_is_live = False

    for index, row in enumerate(rows):
        next_row = rows[index + 1] if index + 1 < len(rows) else None
        row_len = len(row)
        would_exceed_rows = rows_in_pattern >= rows_per_pattern
        would_exceed_bytes = len(packed) + row_len + 1 > max_pattern_bytes
        would_consume_gateoff_room = note_is_live and (
            rows_in_pattern + 2 > rows_per_pattern
            or len(packed) + row_len + 2 > max_pattern_bytes
        )

        # Keep a note-on and its immediately following gate-off in the same
        # pattern. Tempo BigFX rows can shift packed byte boundaries just enough
        # that a pattern would otherwise end with a live note and leave the
        # release buried in the next pattern after silence, causing audible rings.
        would_strand_gateoff = (
            is_note_on(row)
            and is_gate_off(next_row)
            and (
                rows_in_pattern + 2 > rows_per_pattern
                or len(packed) + row_len + len(next_row) + 1 > max_pattern_bytes
            )
        )

        if packed and (would_exceed_rows or would_exceed_bytes or would_consume_gateoff_room or would_strand_gateoff):
            finish_pattern()

        if row_len + 2 > max_pattern_bytes:
            raise ValueError("single row cannot fit in max_pattern_bytes")
        packed.extend(row)
        rows_in_pattern += 1
        if is_note_on(row):
            note_is_live = True
        elif is_gate_off(row):
            note_is_live = False

    finish_pattern()
    return patterns


def pattern_is_silent(pattern: list[int]) -> bool:
    """Return true for a generated pattern that contains only rest rows."""
    for value in pattern:
        if value == SIDWIZARD_PATTERN_END:
            return True
        if value != 0x00:
            return False
    return True


def make_silence_pattern(rows_per_pattern: int, max_pattern_bytes: int) -> list[int]:
    """Build a real pattern that consumes one song step while keeping the voice quiet."""
    rest_rows = max(1, min(rows_per_pattern, max_pattern_bytes - 1))
    pattern = [0x00] * rest_rows
    pattern[0] = SIDWIZARD_GATE_OFF
    pattern.append(SIDWIZARD_PATTERN_END)
    return pattern


def parse_hex_byte_list(text: str) -> list[int]:
    values: list[int] = []
    for token in text.replace(",", " ").split():
        try:
            value = int(token, 16)
        except ValueError:
            continue
        if 0 <= value <= 0xFF:
            values.append(value)
    return values


def order_list_used_patterns(values: list[int]) -> set[int]:
    used: set[int] = set()
    for value in values:
        if value in (0xFE, 0xFF):
            break
        if 1 <= value <= MAX_SIDWIZARD_PATTERN:
            used.add(value)
    return used


def ensure_patterns_end_with_gate_off(patterns: list[list[int]], max_pattern_bytes: int) -> None:
    """Force a release if truncation cuts off the MIDI Note_off event.

    If a track is truncated at SID-Wizard's 100-pattern ceiling, the final
    retained pattern can otherwise end with a note-on row immediately followed
    by $FF. That leaves the SID gate high after the last imported song step.
    """
    for pattern in reversed(patterns):
        if pattern_is_silent(pattern):
            continue

        terminator = pattern.index(SIDWIZARD_PATTERN_END) if SIDWIZARD_PATTERN_END in pattern else len(pattern)
        if terminator > 0 and pattern[terminator - 1] == SIDWIZARD_GATE_OFF:
            return

        if len(pattern) < max_pattern_bytes:
            pattern.insert(terminator, SIDWIZARD_GATE_OFF)
            return

        for index in range(terminator - 1, -1, -1):
            if pattern[index] == 0x00:
                pattern[index] = SIDWIZARD_GATE_OFF
                return

        if terminator > 0:
            pattern[terminator - 1] = SIDWIZARD_GATE_OFF
        return


def hex_bytes(values: list[int]) -> str:
    return " ".join(f"{value:02X}" for value in values)


def shell_quote(text: str) -> str:
    return shlex.quote(text)


def default_sidwiz_path() -> str:
    here = Path(__file__).resolve().parent
    candidates = [
        here / "sidwiz",
        here.parent / "sidwiz",
        here.parent.parent / "build" / "tools" / "sidwiz",
        Path("../build/tools/sidwiz"),
    ]
    for candidate in candidates:
        if candidate.exists():
            return str(candidate)
    return "sidwiz"


def sidwiz_command(args: argparse.Namespace) -> list[str]:
    command = [args.sidwiz]
    if args.sidwiz_labels:
        command.extend(["--labels", args.sidwiz_labels])
    return command


def pattern_number_for_step(args: argparse.Namespace, step: int, voice: int, start_pattern: int) -> int:
    if not args.interleaved_patterns:
        return start_pattern + step
    return args.start_pattern + (step * args.sid_voices) + (voice - 1)


def order_terminator(args: argparse.Namespace) -> list[int]:
    return [0xFF, 0x00] if args.loop else [0xFE]


def read_existing_used_patterns(args: argparse.Namespace) -> set[int]:
    sidwiz = sidwiz_command(args)
    used: set[int] = set()
    for voice in range(1, args.sid_voices + 1):
        # This import replaces the target voice's order list, so its previous
        # pattern references should not block reusing those same pattern slots.
        if voice == args.voice or args.clear_other_voices:
            continue
        result = subprocess.run(
            sidwiz + ["--get-order-list", str(voice)],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        used.update(order_list_used_patterns(parse_hex_byte_list(result.stdout)))
    return used


def choose_pattern_numbers(
    args: argparse.Namespace,
    patterns: list[list[int]],
    used_patterns: set[int],
) -> tuple[dict[int, int], int | None, list[list[int]], bool]:
    assignments: dict[int, int] = {}
    truncated = False
    next_pattern = args.start_pattern
    kept_patterns = list(patterns)
    needs_silence_pattern = args.clear_other_voices or any(pattern_is_silent(pattern) for pattern in patterns)
    silence_pattern = args.silence_pattern if needs_silence_pattern else None

    for index, pattern in enumerate(patterns):
        if pattern_is_silent(pattern):
            continue

        if args.interleaved_patterns:
            pattern_no = pattern_number_for_step(args, index, args.voice, args.start_pattern)
            if pattern_no > MAX_SIDWIZARD_PATTERN:
                truncated = True
                kept_patterns = patterns[:index]
                break
            if pattern_no in used_patterns and not args.clear_song:
                raise ValueError(f"pattern {pattern_no} is already referenced by the song; choose another --start-pattern or omit --interleaved-patterns")
        else:
            while (next_pattern in used_patterns or next_pattern == silence_pattern) and next_pattern <= MAX_SIDWIZARD_PATTERN:
                next_pattern += 1
            if next_pattern > MAX_SIDWIZARD_PATTERN:
                truncated = True
                kept_patterns = patterns[:index]
                break
            pattern_no = next_pattern
            next_pattern += 1

        assignments[index] = pattern_no

    return assignments, silence_pattern, kept_patterns, truncated


def make_commands(args: argparse.Namespace, patterns: list[list[int]], pattern_numbers: dict[int, int], silence_pattern: int | None, drum_instruments: dict[int, int] | None = None) -> list[list[str]]:
    commands: list[list[str]] = []
    sidwiz = sidwiz_command(args)
    if args.clear_song:
        commands.append(sidwiz + ["--clear-song"])

    if drum_instruments:
        for note, instrument in sorted(drum_instruments.items()):
            commands.append(sidwiz + ["--set-inst-name", str(instrument), drum_instrument_name(note)])

    if silence_pattern is not None:
        commands.append(sidwiz + ["--set-pattern-data", str(silence_pattern), hex_bytes(make_silence_pattern(args.rows_per_pattern, args.max_pattern_bytes))])

    for index, pattern in enumerate(patterns):
        if pattern_is_silent(pattern):
            continue
        pattern_no = pattern_numbers[index]
        commands.append(sidwiz + ["--set-pattern-data", str(pattern_no), hex_bytes(pattern)])

    order = [
        silence_pattern if pattern_is_silent(pattern) else pattern_numbers[i]
        for i, pattern in enumerate(patterns)
    ]
    commands.append(sidwiz + ["--set-order-list", str(args.voice), hex_bytes(order + order_terminator(args))])

    if args.clear_other_voices:
        empty_order = [silence_pattern] * len(patterns)
        for voice in range(1, args.sid_voices + 1):
            if voice != args.voice:
                commands.append(sidwiz + ["--set-order-list", str(voice), hex_bytes(empty_order + order_terminator(args))])
    return commands


def print_summary(args: argparse.Namespace, division: int | None, events: list[MidiNoteEvent], tempo_events: list[MidiTempoEvent], rows: list[list[int]], patterns: list[list[int]], ticks_per_row: float, rows_per_quarter: float, row_tempos: dict[int, int], pattern_numbers: dict[int, int], used_patterns: set[int], drum_instruments: dict[int, int] | None = None, drum_collisions: int = 0) -> None:
    first_tick = min((e.tick for e in events), default=0)
    last_tick = max((e.tick for e in events), default=0)
    note_ons = sum(1 for e in events if e.kind == "on")
    note_offs = sum(1 for e in events if e.kind == "off")
    print(f"input={args.csv}")
    print(f"channel={args.channel} track={args.track if args.track is not None else 'any'} division={division if division is not None else 'unknown'} ticks_per_row={ticks_per_row:g} rows_per_quarter={rows_per_quarter:g}")
    print(f"events note_on={note_ons} note_off={note_offs} first_tick={first_tick} last_tick={last_tick}")
    if tempo_events:
        start_tick = start_tick_for_events(events, args.preserve_initial_silence)
        summary_items = []
        for event in tempo_events[:8]:
            row = 0 if event.tick < start_tick else max(0, row_for_tick(event.tick, start_tick, ticks_per_row))
            tempo = row_tempos.get(row)
            bpm = 60_000_000.0 / event.microseconds_per_quarter
            summary_items.append(f"tick {event.tick}:row {row}:tempo {tempo if tempo is not None else '?'} ({bpm:.2f} BPM)")
        tempo_summary = ", ".join(summary_items)
        extra = "" if len(tempo_events) <= 8 else f", ... ({len(tempo_events)} total)"
        print(f"midi_tempos={'ignored' if args.no_midi_tempo else 'applied'} machine={args.machine.upper()} framespeed={args.framespeed} tempo_events={tempo_summary}{extra}")
    print(f"rows={len(rows)} patterns={len(patterns)} rows_per_pattern_max={args.rows_per_pattern} max_pattern_bytes={args.max_pattern_bytes} start_pattern={args.start_pattern} voice={args.voice} instrument={args.instrument} layout={'interleaved' if args.interleaved_patterns else 'compact'} clear_song={args.clear_song} clear_other_voices={args.clear_other_voices}")
    if any(pattern_is_silent(pattern) for pattern in patterns) or args.clear_other_voices:
        print(f"silence_pattern={args.silence_pattern:02X}")
    if pattern_numbers:
        assigned = ", ".join(f"{step + 1}->{pattern_no:02X}" for step, pattern_no in sorted(pattern_numbers.items()))
        print(f"assigned_patterns={assigned}")
    if used_patterns:
        print(f"existing_patterns_seen={len(used_patterns)}")
    if drum_instruments:
        mapping = ", ".join(f"{note}:{instrument}:{drum_instrument_name(note).strip()}" for note, instrument in sorted(drum_instruments.items()))
        print(f"drum_map={mapping}")
        if drum_collisions:
            print(f"warning: {drum_collisions} quantized drum hits collided on an already-used SID-Wizard row; the later hit won")


def run_commands(commands: list[list[str]]) -> None:
    for command in commands:
        print("$ " + " ".join(shell_quote(part) for part in command), flush=True)
        subprocess.run(command, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Convert one midicsv channel to SID-Wizard pattern/order data.")
    parser.add_argument("csv", type=Path, help="midicsv output file")
    parser.add_argument("--channel", type=int, required=True, help="0-based MIDI channel from midicsv")
    parser.add_argument("--track", type=int, help="optional midicsv track number to restrict input")
    parser.add_argument("--voice", type=int, default=1, help="SID-Wizard voice/order list to write, default 1")
    parser.add_argument("--sid-voices", type=int, default=3, choices=(3, 6, 9, 12), help="voices in the running SID-Wizard build, default 3")
    parser.add_argument("--instrument", type=int, default=1, help="SID-Wizard instrument slot, default 1")
    parser.add_argument("--start-pattern", type=int, help="lowest SID-Wizard pattern number to allocate from; when omitted, existing order lists are scanned and free patterns are chosen automatically")
    parser.add_argument("--rows-per-pattern", type=int, default=MAX_SIDWIZARD_ROWS_PER_PATTERN, help=f"maximum rows per generated pattern, default {MAX_SIDWIZARD_ROWS_PER_PATTERN}")
    parser.add_argument("--max-pattern-bytes", type=int, default=MAX_SIDWIZARD_PATTERN_BYTES, help=f"maximum packed bytes per pattern command, including $FF, default {MAX_SIDWIZARD_PATTERN_BYTES}")
    parser.add_argument("--rows-per-quarter", type=float, default=DEFAULT_ROWS_PER_QUARTER, help="quantization when --ticks-per-row is omitted, default 16")
    parser.add_argument("--ticks-per-row", type=float, help="explicit MIDI ticks per SID-Wizard row")
    parser.add_argument("--no-midi-tempo", action="store_true", help="ignore midicsv Tempo events instead of emitting SID-Wizard $10 tempo effects")
    parser.add_argument("--machine", choices=("pal", "ntsc"), default="pal", help="video timing used to convert MIDI tempo to SID-Wizard frames/row, default pal")
    parser.add_argument("--framespeed", type=int, default=1, help="SID-Wizard framespeed multiplier used for tempo conversion, default 1")
    parser.add_argument("--allow-fast-tempo", action="store_true", help="allow generated SID-Wizard tempo values 1..2; otherwise clamp to 3 for normal player compatibility")
    parser.add_argument("--note-offset", type=int, default=-11, help="SID-Wizard note = MIDI note + offset, default -11")
    parser.add_argument("--min-note-rows", type=int, default=1, help="minimum rows between note-on and note-off, default 1")
    parser.add_argument("--preserve-initial-silence", action="store_true", help="do not shift first selected event to row 0")
    parser.add_argument("--loop", action="store_true", help="write order list as looping instead of ending")
    parser.add_argument("--clear-song", action="store_true", help="run sidwiz --clear-song before writing generated patterns")
    parser.add_argument("--clear-other-voices", action="store_true", help="write explicit empty patterns/order lists for the two non-target voices")
    parser.add_argument("--sequential-patterns", action="store_true", help="deprecated compatibility option; compact pattern numbering is now the default")
    parser.add_argument("--interleaved-patterns", action="store_true", help="space pattern numbers by --sid-voices, matching voice/order columns visually but using more pattern slots")
    parser.add_argument("--empty-pattern", type=int, default=100, help="empty pattern used only with --sequential-patterns --clear-other-voices, default 100")
    parser.add_argument("--silence-pattern", type=int, default=MAX_SIDWIZARD_PATTERN, help="reserved real pattern used for generated silence, default 100 ($64)")
    parser.add_argument("--sidwiz", default=default_sidwiz_path(), help="sidwiz executable path")
    parser.add_argument("--sidwiz-labels", default=os.environ.get("SIDWIZ_LABELS") or os.environ.get("SIDWIZARD_LABELS"), help="SID-Wizard label file passed to sidwiz --labels; defaults to SIDWIZ_LABELS/SIDWIZARD_LABELS")
    parser.add_argument("--apply", action="store_true", help="run sidwiz commands instead of only printing them")
    parser.add_argument("--commands-only", action="store_true", help="only print commands, no summary")
    parser.add_argument("--truncate-to-fit", action="store_true", help="truncate generated song steps to fit SID-Wizard's 100-pattern limit")
    parser.add_argument("--drum-map", action="store_true", help="treat selected channel as GM drums: assign each distinct note to consecutive instrument slots")
    parser.add_argument("--drum-play-note", type=int, default=DEFAULT_DRUM_PLAY_NOTE, help="MIDI note used for generated drum hits unless --drum-preserve-pitch is set, default 60/C-5")
    parser.add_argument("--drum-preserve-pitch", action="store_true", help="use each source drum note as the SID-Wizard note pitch instead of --drum-play-note")
    args = parser.parse_args()

    if args.rows_per_pattern > MAX_SIDWIZARD_ROWS_PER_PATTERN:
        parser.error(f"--rows-per-pattern cannot exceed SID-Wizard max {MAX_SIDWIZARD_ROWS_PER_PATTERN}")
    if args.max_pattern_bytes > MAX_SIDWIZARD_PATTERN_BYTES:
        parser.error(f"--max-pattern-bytes cannot exceed SID-Wizard max {MAX_SIDWIZARD_PATTERN_BYTES}")
    if not 1 <= args.instrument <= 0x3E:
        parser.error("--instrument must be 1..62")
    if not 1 <= args.voice <= args.sid_voices:
        parser.error(f"--voice must be 1..{args.sid_voices}")
    if args.start_pattern is not None and not 1 <= args.start_pattern <= MAX_SIDWIZARD_PATTERN:
        parser.error(f"--start-pattern must be 1..{MAX_SIDWIZARD_PATTERN}")
    if not 1 <= args.empty_pattern <= 100:
        parser.error("--empty-pattern must be 1..100")
    if not 1 <= args.silence_pattern <= MAX_SIDWIZARD_PATTERN:
        parser.error(f"--silence-pattern must be 1..{MAX_SIDWIZARD_PATTERN}")
    if not 1 <= args.framespeed <= 8:
        parser.error("--framespeed must be 1..8")

    division, events, tempo_events = parse_midicsv(args.csv, args.channel, args.track)
    if not events:
        print("No matching note events found", file=sys.stderr)
        return 1

    ticks_per_row = args.ticks_per_row
    if ticks_per_row is None:
        if division is None:
            parser.error("CSV has no Header division; specify --ticks-per-row")
        ticks_per_row = division / args.rows_per_quarter
    if ticks_per_row <= 0:
        parser.error("ticks per row must be positive")
    effective_rows_per_quarter = args.rows_per_quarter
    if args.ticks_per_row is not None and division is not None:
        effective_rows_per_quarter = division / ticks_per_row
    frames_per_second = PAL_FRAMES_PER_SECOND if args.machine == "pal" else NTSC_FRAMES_PER_SECOND
    minimum_tempo = 1 if args.allow_fast_tempo else 3
    row_tempos: dict[int, int] = {}
    if not args.no_midi_tempo:
        start_tick = start_tick_for_events(events, args.preserve_initial_silence)
        row_tempos = tempo_events_for_rows(
            tempo_events,
            start_tick,
            ticks_per_row,
            effective_rows_per_quarter,
            frames_per_second,
            args.framespeed,
            minimum_tempo,
        )

    drum_instruments: dict[int, int] | None = None
    drum_collisions = 0
    if args.drum_map:
        drum_instruments = collect_drum_instruments(events, args.instrument)
        if not drum_instruments:
            print("No drum note-on events found", file=sys.stderr)
            return 1
        max_drum_instrument = max(drum_instruments.values())
        if max_drum_instrument > MAX_SIDWIZARD_INSTRUMENT:
            parser.error(f"drum map needs instrument slots through {max_drum_instrument}, exceeding {MAX_SIDWIZARD_INSTRUMENT}")
        rows, drum_collisions = build_drum_rows(
            events,
            ticks_per_row,
            drum_instruments,
            args.note_offset,
            max(0, args.min_note_rows),
            args.preserve_initial_silence,
            args.drum_play_note,
            args.drum_preserve_pitch,
        )
    else:
        rows = build_rows(
            events,
            ticks_per_row,
            args.instrument,
            args.note_offset,
            max(0, args.min_note_rows),
            args.preserve_initial_silence,
        )
    rows = apply_tempo_events(rows, row_tempos)
    patterns = split_patterns(rows, args.rows_per_pattern, args.max_pattern_bytes)
    if args.start_pattern is None:
        args.start_pattern = 1
    if args.interleaved_patterns and pattern_number_for_step(args, 0, args.voice, args.start_pattern) > MAX_SIDWIZARD_PATTERN:
        parser.error(f"start pattern {args.start_pattern} and voice {args.voice} leave no usable patterns with this layout")

    try:
        used_patterns = set() if args.clear_song else read_existing_used_patterns(args)
    except (subprocess.CalledProcessError, FileNotFoundError) as exc:
        parser.error(f"could not read existing SID-Wizard order lists for automatic pattern allocation: {exc}")

    patterns_before_fit = len(patterns)
    try:
        pattern_numbers, silence_pattern, fit_patterns, truncated = choose_pattern_numbers(args, patterns, used_patterns)
    except ValueError as exc:
        parser.error(str(exc))
    if truncated:
        if args.truncate_to_fit:
            print(
                f"warning: truncating generated song from {patterns_before_fit} to {len(fit_patterns)} steps to fit SID-Wizard pattern 100",
                file=sys.stderr,
            )
            patterns = fit_patterns
            ensure_patterns_end_with_gate_off(patterns, args.max_pattern_bytes)
            pattern_numbers = {step: pattern for step, pattern in pattern_numbers.items() if step < len(patterns)}
        else:
            parser.error(f"generated song needs more free patterns than are available through SID-Wizard pattern {MAX_SIDWIZARD_PATTERN}; use --truncate-to-fit to keep the portion that fits")
    max_pattern_no = max(pattern_numbers.values(), default=0)
    if not args.interleaved_patterns and args.clear_other_voices and args.start_pattern <= args.empty_pattern <= max_pattern_no:
        parser.error("--empty-pattern overlaps generated target patterns")

    commands = make_commands(args, patterns, pattern_numbers, silence_pattern, drum_instruments)
    if not args.commands_only:
        print_summary(args, division, events, tempo_events, rows, patterns, ticks_per_row, effective_rows_per_quarter, row_tempos, pattern_numbers, used_patterns, drum_instruments, drum_collisions)
    if args.apply:
        run_commands(commands)
    else:
        for command in commands:
            print(" ".join(shell_quote(part) for part in command))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
