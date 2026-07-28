#!/usr/bin/env python3
# Sysop-64
# https://github.com/Bloodmosher/Sysop-64
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Sysop-64 Project

"""
Filter a raw tickstream capture to C64 writes and send frame-oriented replay data.
"""

import argparse
import mmap
import socket
import struct
import sys
import time
from collections import OrderedDict


MAGIC = b"S64RPLY2"
SAMPLE_BYTES = 8
RECORD_BYTES = 32
ADDR_0001 = 0x0001
ADDR_D020 = 0xD020
ADDR_D011 = 0xD011
D011_DEN = 0x10


def sample_offset(index, reverse_lanes):
    if not reverse_lanes:
        return index * SAMPLE_BYTES

    record = index // 4
    lane = index & 3
    return record * RECORD_BYTES + (3 - lane) * SAMPLE_BYTES


def decode(raw):
    return {
        "data": raw & 0xFF,
        "addr": (raw >> 8) & 0xFFFF,
        "r_w": (raw >> 24) & 1,
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

    parts = text.lower().replace("0x", "").split("-", 1)
    start = int(parts[0], 16)
    end = int(parts[1] if len(parts) == 2 else parts[0], 16)
    return min(start, end), max(start, end)


def frame_slot(sample, cycles):
    cycle = sample["cycle"]
    if cycle < 1:
        cycle = 1
    elif cycle > cycles:
        cycle = cycles
    return sample["line"] * cycles + (cycle - 1)


def range_matches(addr, addr_range):
    return addr_range is None or addr_range[0] <= addr <= addr_range[1]


def is_dec_0001_d020_inc_0001(events):
    return (
        len(events) == 3
        and events[0][3] == ADDR_0001
        and events[1][3] == ADDR_D020
        and events[2][3] == ADDR_0001
    )


def is_delayed_0001_d020_restore(events):
    return (
        len(events) == 4
        and events[0][3] == ADDR_0001
        and events[1][3] == ADDR_0001
        and events[2][3] == ADDR_D020
        and events[3][3] == ADDR_0001
        and events[1][4] == events[3][4]
    )


def is_delayed_0001_d020_prefix(events):
    return (
        len(events) == 3
        and events[0][3] == ADDR_0001
        and events[1][3] == ADDR_0001
        and events[2][3] == ADDR_D020
    )


def is_d020_delayed_0001_pair(events):
    return (
        len(events) == 3
        and events[0][3] == ADDR_D020
        and events[1][3] == ADDR_0001
        and events[2][3] == ADDR_0001
    )


class WriteFilter:
    def __init__(self):
        self.pending = []
        self.dropped_transient_border_writes = 0

    def push(self, event):
        return [event]

    def flush(self):
        ready = self.pending
        self.pending = []
        return ready


def iter_frames(path, lines, cycles, addr_range, reverse_lanes, max_frames, start_sample=0):
    pending = []
    write_filter = WriteFilter()
    current_frame = None
    emitted_frames = 0

    with open(path, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        total_samples = len(mm) // SAMPLE_BYTES

        try:
            start_sample = max(0, min(start_sample, total_samples))
            for index in range(start_sample, total_samples):
                offset = sample_offset(index, reverse_lanes)
                if offset + SAMPLE_BYTES > len(mm):
                    break

                raw = int.from_bytes(mm[offset:offset + SAMPLE_BYTES], "little")
                sample = decode(raw)

                if sample["line"] >= lines:
                    continue

                slot = frame_slot(sample, cycles)
                sample_frame = sample["frame"]
                if current_frame is None:
                    current_frame = sample_frame
                elif sample_frame != current_frame:
                    for event in write_filter.flush():
                        if range_matches(event[3], addr_range):
                            pending.append(event)
                    pending.sort(key=lambda item: item[0])
                    yield current_frame, pending
                    emitted_frames += 1
                    if max_frames is not None and emitted_frames >= max_frames:
                        return
                    pending = []
                    current_frame = sample_frame

                if sample["r_w"] != 0:
                    continue

                event = (slot, sample["line"], sample["cycle"], sample["addr"], sample["data"])
                for ready in write_filter.push(event):
                    if range_matches(ready[3], addr_range):
                        pending.append(ready)
        finally:
            mm.close()

    for event in write_filter.flush():
        if range_matches(event[3], addr_range):
            pending.append(event)

    if current_frame is not None and (pending or max_frames is None) and (max_frames is None or emitted_frames < max_frames):
        pending.sort(key=lambda item: item[0])
        yield current_frame, pending


def slot_to_line_cycle(slot, cycles):
    return slot // cycles, (slot % cycles) + 1


def build_fast_forward_events(path, sample_count, lines, cycles, addr_range, reverse_lanes):
    if sample_count <= 0:
        return [], 0, 0

    mirrors = OrderedDict()
    write_filter = WriteFilter()
    current_map = 0x37
    mirrors[current_map] = OrderedDict()
    scanned_samples = 0
    raw_writes = 0

    def remember_event(event):
        nonlocal current_map
        _, _, _, addr, data = event

        if addr == ADDR_0001:
            current_map = data
            mirrors.setdefault(current_map, OrderedDict())

        if not range_matches(addr, addr_range):
            return

        mirror = mirrors.setdefault(current_map, OrderedDict())
        if addr in mirror:
            del mirror[addr]
        mirror[addr] = data

    with open(path, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        total_samples = len(mm) // SAMPLE_BYTES
        limit = min(sample_count, total_samples)

        try:
            for index in range(limit):
                offset = sample_offset(index, reverse_lanes)
                if offset + SAMPLE_BYTES > len(mm):
                    break

                scanned_samples += 1
                sample = decode(int.from_bytes(mm[offset:offset + SAMPLE_BYTES], "little"))
                if sample["r_w"] != 0:
                    continue

                raw_writes += 1
                event = (
                    frame_slot(sample, cycles),
                    sample["line"],
                    sample["cycle"],
                    sample["addr"],
                    sample["data"],
                )
                for ready in write_filter.push(event):
                    remember_event(ready)
        finally:
            mm.close()

    for ready in write_filter.flush():
        remember_event(ready)

    events = []
    burst_slot = 0
    slots_per_frame = lines * cycles

    def append_burst_event(addr, data):
        nonlocal burst_slot
        line, cycle = slot_to_line_cycle(burst_slot % slots_per_frame, cycles)
        events.append((burst_slot, line, cycle, addr, data))
        burst_slot += 1

    for map_value, mirror in mirrors.items():
        if not mirror:
            continue

        append_burst_event(0x0001, map_value)
        for addr, data in mirror.items():
            if addr == 0x0001:
                continue
            append_burst_event(addr, data)

    return events, scanned_samples, raw_writes


def choose_auto_fast_forward_samples(args):
    last_frame = None
    frame_count = 0
    window_start_sample = 0
    window_writes = 0
    fast_forward_samples = 0
    quiet_windows = 0

    with open(args.capture, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        total_samples = len(mm) // SAMPLE_BYTES

        try:
            for index in range(total_samples):
                offset = sample_offset(index, args.reverse_lanes)
                if offset + SAMPLE_BYTES > len(mm):
                    break

                sample = decode(int.from_bytes(mm[offset:offset + SAMPLE_BYTES], "little"))
                if sample["line"] >= args.lines:
                    continue

                sample_frame = sample["frame"]
                if last_frame is not None and sample_frame != last_frame:
                    frame_count += 1
                    if frame_count >= args.auto_fast_forward_window_frames:
                        bytes_per_second = window_writes * SAMPLE_BYTES * args.fps / frame_count
                        if args.stats:
                            print(
                                "auto-fast-forward window "
                                f"start_sample={window_start_sample} end_sample={index} "
                                f"frames={frame_count} writes={window_writes} "
                                f"rate={bytes_per_second / 1024 / 1024:.2f} MB/s",
                                file=sys.stderr,
                            )

                        if bytes_per_second < args.auto_fast_forward_min_rate:
                            quiet_windows += 1
                            if quiet_windows > args.auto_fast_forward_quiet_windows:
                                return fast_forward_samples
                        else:
                            quiet_windows = 0

                        fast_forward_samples = index
                        if (
                            args.auto_fast_forward_max_samples
                            and fast_forward_samples >= args.auto_fast_forward_max_samples
                        ):
                            return args.auto_fast_forward_max_samples

                        frame_count = 0
                        window_start_sample = index
                        window_writes = 0

                last_frame = sample_frame

                if sample["r_w"] == 0 and range_matches(sample["addr"], args.auto_fast_forward_range):
                    window_writes += 1
        finally:
            mm.close()

    return fast_forward_samples


def choose_d011_blank_fast_forward_samples(args):
    blank_start_sample = None
    blank_start_frame = None

    with open(args.capture, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        total_samples = len(mm) // SAMPLE_BYTES

        try:
            for index in range(total_samples):
                if args.auto_fast_forward_max_samples and index >= args.auto_fast_forward_max_samples:
                    break

                offset = sample_offset(index, args.reverse_lanes)
                if offset + SAMPLE_BYTES > len(mm):
                    break

                sample = decode(int.from_bytes(mm[offset:offset + SAMPLE_BYTES], "little"))
                if sample["r_w"] != 0 or sample["addr"] != ADDR_D011:
                    continue

                screen_enabled = (sample["data"] & D011_DEN) != 0
                if blank_start_sample is None:
                    if not screen_enabled:
                        blank_start_sample = index
                        blank_start_frame = sample["frame"]
                        if args.stats:
                            print(
                                "d011-fast-forward blanked "
                                f"sample={index} frame={sample['frame']} "
                                f"line={sample['line']} cycle={sample['cycle']} "
                                f"value=${sample['data']:02x}",
                                file=sys.stderr,
                            )
                elif screen_enabled:
                    if args.stats:
                        print(
                            "d011-fast-forward restored "
                            f"sample={index} frame={sample['frame']} "
                            f"line={sample['line']} cycle={sample['cycle']} "
                            f"value=${sample['data']:02x} "
                            f"blank_start_sample={blank_start_sample} "
                            f"blank_start_frame={blank_start_frame}",
                            file=sys.stderr,
                        )
                    return index + 1
        finally:
            mm.close()

    if args.stats and blank_start_sample is not None:
        print(
            "d011-fast-forward saw blank but no restore; not fast-forwarding",
            file=sys.stderr,
        )
    return 0


def send_event_frame(sock, frame_delta, events):
    payload = bytearray()
    payload += struct.pack(">H", frame_delta & 0x00FF)
    payload += struct.pack(">I", len(events))
    for _, line, cycle, addr, data in events:
        payload += struct.pack(">HBHB", line, cycle, addr, data)
    sock.sendall(payload)


def send_one_pass(sock, args, addr_range, sent_frames, sent_events):
    pass_frames = 0

    start_sample = 0
    if args.fast_forward_samples:
        events, scanned_samples, raw_writes = build_fast_forward_events(
            args.capture,
            args.fast_forward_samples,
            args.lines,
            args.cycles,
            addr_range,
            args.reverse_lanes,
        )
        if events:
            send_event_frame(sock, 0, events)
            sent_frames += 1
            sent_events += len(events)
            pass_frames += 1
        start_sample = scanned_samples
        if args.stats:
            print(
                "fast-forward "
                f"samples={scanned_samples} raw_writes={raw_writes} "
                f"coalesced_events={len(events)}",
                file=sys.stderr,
            )

    previous_capture_frame = None
    for frame_number, events in iter_frames(
        args.capture,
        args.lines,
        args.cycles,
        addr_range,
        args.reverse_lanes,
        None,
        start_sample,
    ):
        frame_delta = 0 if previous_capture_frame is None else ((frame_number - previous_capture_frame) & 0x00FF)
        send_event_frame(sock, frame_delta, events)
        previous_capture_frame = frame_number
        sent_frames += 1
        sent_events += len(events)
        pass_frames += 1

        if args.pace:
            elapsed_target = sent_frames / args.fps
            elapsed = time.monotonic() - args.start_time
            if elapsed_target > elapsed:
                time.sleep(elapsed_target - elapsed)

        if args.stats and sent_frames % args.stats == 0:
            print(
                f"sent capture_frame={frame_number} frames={sent_frames} events={sent_events}",
                file=sys.stderr,
            )

        if args.max_frames is not None and sent_frames >= args.max_frames:
            return sent_frames, sent_events, pass_frames, True

    return sent_frames, sent_events, pass_frames, False


def send_frames(args):
    addr_range = parse_hex_range(args.range)
    args.auto_fast_forward_range = addr_range

    if args.auto_fast_forward_d011_blank:
        args.fast_forward_samples = choose_d011_blank_fast_forward_samples(args)
        print(
            f"d011-fast-forward selected samples={args.fast_forward_samples}",
            file=sys.stderr,
        )
    elif args.auto_fast_forward:
        args.fast_forward_samples = choose_auto_fast_forward_samples(args)
        print(
            f"auto-fast-forward selected samples={args.fast_forward_samples}",
            file=sys.stderr,
        )

    with socket.create_connection((args.host, args.port)) as sock:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.sendall(MAGIC + struct.pack(">HH", args.lines, args.cycles))

        sent_frames = 0
        sent_events = 0
        args.start_time = time.monotonic()

        while True:
            sent_frames, sent_events, pass_frames, hit_limit = send_one_pass(
                sock,
                args,
                addr_range,
                sent_frames,
                sent_events,
            )

            if hit_limit:
                break
            if pass_frames == 0:
                print("no frames found in capture", file=sys.stderr)
                break
            if not args.loop:
                break

    print(f"done: frames={sent_frames} events={sent_events}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description="Send filtered tickstream writes to tickreplay_receiver.")
    parser.add_argument("capture", help="raw tickstream capture file")
    parser.add_argument("host", help="receiver host/IP")
    parser.add_argument("--port", type=int, default=9100)
    parser.add_argument("--lines", type=int, default=312)
    parser.add_argument("--cycles", type=int, default=63)
    parser.add_argument("--fps", type=float, default=50.0)
    parser.add_argument("--pace", action="store_true", help="pace sender at --fps instead of sending as fast as possible")
    parser.add_argument("--loop", action="store_true", help="restart the capture when the end is reached")
    parser.add_argument("--range", help="hex address/range filter, e.g. d000-d41f")
    parser.add_argument("--reverse-lanes", action="store_true")
    parser.add_argument("--max-frames", type=int)
    parser.add_argument("--stats", type=int, default=50, help="print stats every N frames, 0 disables")
    parser.add_argument(
        "--fast-forward-samples",
        type=int,
        default=0,
        help=(
            "coalesce the first N capture samples into memory-map catch-up bursts "
            "before timed replay starts"
        ),
    )
    parser.add_argument(
        "--auto-fast-forward",
        action="store_true",
        help="choose --fast-forward-samples automatically from the initial write rate",
    )
    parser.add_argument(
        "--auto-fast-forward-d011-blank",
        action="store_true",
        help="fast-forward from the first D011 screen-blank write through the next screen-enable write",
    )
    parser.add_argument(
        "--auto-fast-forward-min-rate",
        type=float,
        default=1024 * 1024,
        help="continue auto fast-forwarding while the selected write stream is at least this many bytes/sec",
    )
    parser.add_argument(
        "--auto-fast-forward-window-frames",
        type=int,
        default=100,
        help="number of raster frames per auto fast-forward rate window",
    )
    parser.add_argument(
        "--auto-fast-forward-quiet-windows",
        type=int,
        default=0,
        help="below-threshold windows to bridge before stopping auto fast-forward",
    )
    parser.add_argument(
        "--auto-fast-forward-max-samples",
        type=int,
        default=0,
        help="optional cap for automatically selected fast-forward samples, 0 disables",
    )
    args = parser.parse_args()

    if args.lines <= 0 or args.cycles <= 0:
        raise SystemExit("lines and cycles must be positive")
    if args.port < 1 or args.port > 65535:
        raise SystemExit("port must be between 1 and 65535")
    if args.fast_forward_samples < 0:
        raise SystemExit("--fast-forward-samples must be non-negative")
    if args.auto_fast_forward and args.fast_forward_samples:
        raise SystemExit("use either --auto-fast-forward or --fast-forward-samples, not both")
    if args.auto_fast_forward_min_rate < 0:
        raise SystemExit("--auto-fast-forward-min-rate must be non-negative")
    if args.auto_fast_forward_window_frames <= 0:
        raise SystemExit("--auto-fast-forward-window-frames must be positive")
    if args.auto_fast_forward_quiet_windows < 0:
        raise SystemExit("--auto-fast-forward-quiet-windows must be non-negative")
    if args.auto_fast_forward_max_samples < 0:
        raise SystemExit("--auto-fast-forward-max-samples must be non-negative")

    send_frames(args)


if __name__ == "__main__":
    main()
