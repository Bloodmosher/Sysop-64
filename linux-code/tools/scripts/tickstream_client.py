#!/usr/bin/env python3
# Sysop-64
# https://github.com/Bloodmosher/Sysop-64
#
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Sysop-64 Project

"""
Connect to tickstream, receive raw c64_tick_sampler data, and write it to disk.

The stream is intentionally treated as opaque bytes in the receive loop.  Decode
or analyze the 64-bit samples later so the receiver can keep up with capture.
"""

import argparse
import os
import socket
import sys
import time


DEFAULT_CHUNK_BYTES = 1024 * 1024
DEFAULT_STATS_SECONDS = 1.0
SOCKET_TIMEOUT_SECONDS = 0.5


def format_rate(bytes_per_second):
    units = ("B/s", "KB/s", "MB/s", "GB/s")
    value = float(bytes_per_second)

    for unit in units:
        if value < 1024.0 or unit == units[-1]:
            return f"{value:.2f} {unit}"
        value /= 1024.0


def receive_stream(args):
    buffer = bytearray(args.chunk_bytes)
    view = memoryview(buffer)
    total_bytes = 0
    last_total = 0
    start_time = time.monotonic()
    last_stats_time = start_time

    with socket.create_connection((args.host, args.port)) as conn:
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        conn.settimeout(SOCKET_TIMEOUT_SECONDS)
        print(f"connected to {args.host}:{args.port}", file=sys.stderr)

        with open(args.output, "wb", buffering=args.file_buffer_bytes) as output:
            while True:
                try:
                    received = conn.recv_into(view)
                except socket.timeout:
                    continue

                if received == 0:
                    break

                output.write(view[:received])
                total_bytes += received

                now = time.monotonic()
                if args.stats and now - last_stats_time >= args.stats_seconds:
                    interval = now - last_stats_time
                    interval_bytes = total_bytes - last_total
                    elapsed = now - start_time
                    print(
                        f"{total_bytes} bytes "
                        f"elapsed={elapsed:.1f}s "
                        f"rate={format_rate(interval_bytes / interval)}",
                        file=sys.stderr,
                    )
                    last_stats_time = now
                    last_total = total_bytes

    elapsed = max(time.monotonic() - start_time, 0.001)
    print(
        f"done: {total_bytes} bytes in {elapsed:.1f}s "
        f"avg={format_rate(total_bytes / elapsed)}",
        file=sys.stderr,
    )


def parse_args():
    parser = argparse.ArgumentParser(
        description="Receive tickstream TCP data and save the raw capture."
    )
    parser.add_argument(
        "output",
        help="output capture file",
    )
    parser.add_argument(
        "host",
        help="tickstream server host/IP",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=9000,
        help="TCP port to connect to, default: 9000",
    )
    parser.add_argument(
        "--chunk-bytes",
        type=int,
        default=DEFAULT_CHUNK_BYTES,
        help=f"socket receive buffer size, default: {DEFAULT_CHUNK_BYTES}",
    )
    parser.add_argument(
        "--file-buffer-bytes",
        type=int,
        default=4 * DEFAULT_CHUNK_BYTES,
        help="Python file buffer size, default: 4194304",
    )
    parser.add_argument(
        "--stats-seconds",
        type=float,
        default=DEFAULT_STATS_SECONDS,
        help=f"seconds between throughput updates, default: {DEFAULT_STATS_SECONDS}",
    )
    parser.add_argument(
        "--no-stats",
        dest="stats",
        action="store_false",
        help="disable periodic throughput output",
    )
    parser.set_defaults(stats=True)
    return parser.parse_args()


def main():
    args = parse_args()

    if args.port < 1 or args.port > 65535:
        raise SystemExit("port must be between 1 and 65535")
    if args.chunk_bytes <= 0:
        raise SystemExit("chunk-bytes must be positive")
    if args.file_buffer_bytes <= 0:
        raise SystemExit("file-buffer-bytes must be positive")

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    try:
        receive_stream(args)
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)


if __name__ == "__main__":
    main()
