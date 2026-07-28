#!/usr/bin/env python3
# Sysop-64
# https://github.com/Bloodmosher/Sysop-64
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Sysop-64 Project

"""
Analyze c64_tick_sampler captures for inferred $0001 vs CHL mismatches.

The trace format is the raw tickstream format: little-endian 64-bit samples,
four samples per 32-byte SDRAM writer record.  Bits 63:56 hold the frame, bits 55/54 hold _irq/_dma, and bit 53 is expected to contain
the c64_tick_sampler "$01=CHL" match flag.
"""

import argparse
import struct
from collections import Counter, deque


SAMPLE_BYTES = 8
RECORD_BYTES = 32
ADDR_0001 = 0x0001
RESET_VECTOR_LO = 0xFFFC
RESET_VECTOR_HI = 0xFFFD
KERNAL_PORT01_INIT_PC = 0xFDD5
KERNAL_PORT01_INIT_BYTES = (0xA9, 0xE7, 0x85, 0x01)


def sample_offset(index, reverse_lanes):
    if not reverse_lanes:
        return index * SAMPLE_BYTES

    record = index // 4
    lane = index & 3
    return record * RECORD_BYTES + (3 - lane) * SAMPLE_BYTES


def decode(raw):
    return {
        "raw": raw,
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
        "port01_chl_match": (raw >> 53) & 1,
        "dma": (raw >> 54) & 1,
        "irq": (raw >> 55) & 1,
        "frame": (raw >> 56) & 0xFF,
    }


def parse_hex_range(text):
    if text is None:
        return None

    parts = text.lower().replace("$", "").replace("0x", "").split("-", 1)
    start = int(parts[0], 16)
    end = int(parts[1] if len(parts) == 2 else parts[0], 16)
    return min(start, end), max(start, end)


def in_range(sample, addr_range):
    return addr_range is None or addr_range[0] <= sample["addr"] <= addr_range[1]


def is_write(sample):
    return sample["r_w"] == 0


def chl_value(sample):
    return (sample["charen"] << 2) | (sample["hiram"] << 1) | sample["loram"]


def format_sample(index, sample):
    rw = "R" if sample["r_w"] else "W"
    chl = chl_value(sample)
    marker = "!" if sample["port01_chl_match"] == 0 else " "
    return (
        f"{marker}#{index:9d} "
        f"fr={sample['frame']:4d} t={sample['tick']:2d} "
        f"l={sample['line']:3d} c={sample['cycle']:3d} "
        f"{rw} ${sample['addr']:04X}=${sample['data']:02X} "
        f"CHL={chl:X} match={sample['port01_chl_match']} "
        f"phi2={sample['phi2']} ba={sample['ba']} "
        f"irq={sample['irq']} dma={sample['dma']} "
        f"raw=0x{sample['raw']:016X}"
    )


def read_sample_at(trace, index, reverse_lanes):
    offset = sample_offset(index, reverse_lanes)
    if offset + SAMPLE_BYTES > len(trace):
        return None
    raw = struct.unpack_from("<Q", trace, offset)[0]
    return decode(raw)


def find_reset_vectors(trace, total_samples, reverse_lanes, limit):
    hits = []
    for index in range(total_samples):
        sample = read_sample_at(trace, index, reverse_lanes)
        if sample is None:
            break
        if sample["r_w"] and sample["addr"] in (RESET_VECTOR_LO, RESET_VECTOR_HI):
            hits.append((index, sample))
            if len(hits) >= limit:
                break
    return hits


def find_reset_sequence(trace, total_samples, reverse_lanes):
    for index in range(total_samples - 4):
        samples = [read_sample_at(trace, index + offset, reverse_lanes) for offset in range(4)]
        if any(sample is None for sample in samples):
            break

        if all(
            sample["r_w"]
            and sample["addr"] == KERNAL_PORT01_INIT_PC + offset
            and sample["data"] == KERNAL_PORT01_INIT_BYTES[offset]
            for offset, sample in enumerate(samples)
        ):
            write_index = None
            for candidate in range(index + 4, min(total_samples, index + 16)):
                sample = read_sample_at(trace, candidate, reverse_lanes)
                if sample is not None and is_write(sample) and sample["addr"] == ADDR_0001:
                    write_index = candidate
                    break
            return index, write_index

    return None


def print_reset_report(trace, total_samples, args):
    vectors = find_reset_vectors(trace, total_samples, args.reverse_lanes, args.max_reset_vectors)
    reset_sequence = find_reset_sequence(trace, total_samples, args.reverse_lanes)

    print("reset anchor")
    if vectors:
        print(f"  reset_vector_reads={len(vectors)}")
        for index, sample in vectors:
            print(f"  {format_sample(index, sample)}")
    else:
        print("  reset_vector_reads=0")

    if reset_sequence is None:
        print("  kernal_port01_init=not_found")
        return None

    sequence_index, write_index = reset_sequence
    print(
        "  kernal_port01_init=found "
        f"start=#{sequence_index} "
        f"write_0001=#{write_index if write_index is not None else 'not_found'}"
    )

    if args.show_reset_context:
        start = max(0, sequence_index - args.reset_before)
        end_base = write_index if write_index is not None else sequence_index + 3
        end = min(total_samples - 1, end_base + args.reset_after)
        for index in range(start, end + 1):
            sample = read_sample_at(trace, index, args.reverse_lanes)
            if sample is not None:
                print(f"  {format_sample(index, sample)}")

    return reset_sequence


def find_previous_writes(history, limit):
    writes = []
    for index, sample in reversed(history):
        if is_write(sample):
            writes.append((index, sample))
            if len(writes) >= limit:
                break
    writes.reverse()
    return writes


def summarize_nearby_writes(history):
    writes = find_previous_writes(history, 8)
    if not writes:
        return "no previous writes in context"

    parts = []
    for index, sample in writes:
        parts.append(f"#{index}:${sample['addr']:04X}=${sample['data']:02X}/CHL{chl_value(sample):X}")
    return ", ".join(parts)


def print_transition(trace, total_samples, index, sample, history, args):
    print()
    print(f"match fell 1->0 at sample #{index}")
    print(f"previous writes: {summarize_nearby_writes(history)}")

    start = max(0, index - args.before)
    end = min(total_samples - 1, index + args.after)

    for context_index in range(start, end + 1):
        context_sample = read_sample_at(trace, context_index, args.reverse_lanes)
        if context_sample is None:
            continue
        if args.context_writes_only and not is_write(context_sample):
            continue
        if not in_range(context_sample, args.context_range):
            continue
        print(format_sample(context_index, context_sample))


def analyze(args):
    with open(args.capture, "rb") as f:
        trace = f.read()

    total_samples = len(trace) // SAMPLE_BYTES
    trailing_bytes = len(trace) % SAMPLE_BYTES
    if trailing_bytes:
        print(f"warning: ignoring {trailing_bytes} trailing byte(s)")

    reset_sequence = print_reset_report(trace, total_samples, args)

    analysis_start = args.start_sample
    if reset_sequence is not None and not args.ignore_reset_anchor:
        _, write_index = reset_sequence
        if write_index is not None:
            analysis_start = max(analysis_start, write_index + 1)

    print()
    print(f"analysis_start=#{analysis_start}")

    history = deque(maxlen=max(args.before, 32))
    counts = Counter()
    transitions = 0
    previous_match = None
    mismatch_runs = []
    current_mismatch_start = None
    current_mismatch_len = 0
    first_mismatch = None

    for index in range(analysis_start, total_samples):
        if args.max_samples is not None and index >= analysis_start + args.max_samples:
            break

        sample = read_sample_at(trace, index, args.reverse_lanes)
        if sample is None:
            break

        match = sample["port01_chl_match"]
        counts["samples"] += 1
        counts[f"match_{match}"] += 1
        if is_write(sample):
            counts["writes"] += 1
            if sample["addr"] == ADDR_0001:
                counts["writes_0001"] += 1

        if match == 0:
            if first_mismatch is None:
                first_mismatch = (index, sample)
            if current_mismatch_start is None:
                current_mismatch_start = index
                current_mismatch_len = 1
            else:
                current_mismatch_len += 1
        elif current_mismatch_start is not None:
            mismatch_runs.append((current_mismatch_start, current_mismatch_len))
            current_mismatch_start = None
            current_mismatch_len = 0

        if previous_match == 1 and match == 0:
            transitions += 1
            if transitions <= args.max_transitions:
                print_transition(trace, total_samples, index, sample, history, args)

        previous_match = match
        history.append((index, sample))

    if current_mismatch_start is not None:
        mismatch_runs.append((current_mismatch_start, current_mismatch_len))

    print()
    print("summary")
    print(f"  analyzed_from_sample={analysis_start}")
    print(f"  samples_scanned={counts['samples']}")
    print(f"  writes={counts['writes']} writes_to_0001={counts['writes_0001']}")
    print(f"  match_1={counts['match_1']} match_0={counts['match_0']}")
    print(f"  falling_transitions={transitions}")
    print(f"  mismatch_runs={len(mismatch_runs)}")

    if first_mismatch is not None:
        index, sample = first_mismatch
        print(
            "  first_mismatch="
            f"#{index} frame={sample['frame']} line={sample['line']} "
            f"cycle={sample['cycle']} tick={sample['tick']} "
            f"addr=${sample['addr']:04X} data=${sample['data']:02X} "
            f"CHL={chl_value(sample):X}"
        )

    for run_index, (start, length) in enumerate(mismatch_runs[: args.max_runs], 1):
        sample = read_sample_at(trace, start, args.reverse_lanes)
        print(
            f"  run{run_index}: start=#{start} len={length} "
            f"frame={sample['frame']} line={sample['line']} "
            f"cycle={sample['cycle']} tick={sample['tick']}"
        )


def main():
    parser = argparse.ArgumentParser(
        description="Analyze tickstream captures for inferred $0001 vs CHL mismatches."
    )
    parser.add_argument("capture", help="raw tickstream capture file")
    parser.add_argument("--reverse-lanes", action="store_true",
        help="decode each 32-byte SDRAM writer record with lane order reversed")
    parser.add_argument("--start-sample", type=int, default=0,
        help="minimum sample index to begin scanning")
    parser.add_argument("--max-samples", type=int,
        help="maximum number of samples to scan")
    parser.add_argument("--ignore-reset-anchor", action="store_true",
        help="scan from --start-sample instead of beginning after the KERNAL $0001 reset init")
    parser.add_argument("--show-reset-context", action=argparse.BooleanOptionalAction, default=True,
        help="print samples around the detected KERNAL $0001 reset init")
    parser.add_argument("--reset-before", type=int, default=6,
        help="samples to print before the detected KERNAL $0001 reset init")
    parser.add_argument("--reset-after", type=int, default=8,
        help="samples to print after the detected KERNAL $0001 reset init write")
    parser.add_argument("--max-reset-vectors", type=int, default=4,
        help="maximum reset vector reads to print")
    parser.add_argument("--before", type=int, default=24,
        help="samples of context before each 1->0 transition")
    parser.add_argument("--after", type=int, default=24,
        help="samples of context after each 1->0 transition")
    parser.add_argument("--max-transitions", type=int, default=5,
        help="maximum number of 1->0 transitions to print in detail")
    parser.add_argument("--max-runs", type=int, default=8,
        help="maximum mismatch runs to list in the summary")
    parser.add_argument("--context-writes-only", action="store_true",
        help="only print write samples in transition context")
    parser.add_argument("--context-range", type=parse_hex_range,
        help="only print context samples whose address is in hex range, e.g. 0000-00ff")
    args = parser.parse_args()

    analyze(args)


if __name__ == "__main__":
    main()
