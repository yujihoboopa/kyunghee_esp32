#!/usr/bin/env python3
"""Connect to LuaGC-ESP32, subscribe, clock-calibrate, and save notify arrivals."""
import argparse
import asyncio
import csv
import struct
import time
import signal
import math
import statistics
from collections import defaultdict
from pathlib import Path

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "LuaGC-ESP32"
CHAR_UUID = "0200eedd-ccbb-aa89-3847-261534537291"
PROFILE_COUNT = 12
SAMPLES_PER_PROFILE = 100

async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--output", default="results/ble-central.csv")
    ap.add_argument("--summary", default="results/ble-central-summary.csv")
    args = ap.parse_args()
    out = Path(args.output); out.parent.mkdir(parents=True, exist_ok=True)
    summary_path = Path(args.summary); summary_path.parent.mkdir(parents=True, exist_ok=True)

    print(f"Scanning for {DEVICE_NAME}...")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=25.0)
    if device is None:
        raise RuntimeError(f"Could not find BLE device {DEVICE_NAME}")

    seen = defaultdict(set)
    latencies = defaultdict(list)
    profile_start_seen = set()
    profile_closed = set()
    offsets = []
    with out.open("w", newline="", buffering=1) as f:
        writer = csv.writer(f)
        writer.writerow(["host_receive_monotonic_ns", "esp_request_us", "estimated_receive_esp_us",
                         "ble_latency_us", "clock_offset_uncertainty_us", "profile", "sequence", "payload_hex"])
        async with BleakClient(device, timeout=30.0) as client:
            # Estimate central-minus-ESP clock offset from repeated characteristic reads.
            for _ in range(20):
                t0 = time.monotonic_ns()
                value = await client.read_gatt_char(CHAR_UUID)
                t1 = time.monotonic_ns()
                if len(value) != 8:
                    raise RuntimeError(f"Expected 8-byte ESP timer value, got {len(value)} bytes")
                esp_us = struct.unpack("<Q", value)[0]
                rtt_ns = t1 - t0
                offsets.append(((t0 + t1) // 2 - esp_us * 1000, rtt_ns))
            offset_ns, best_rtt_ns = min(offsets, key=lambda pair: pair[1])
            uncertainty_us = (best_rtt_ns + 1_999) // 2_000
            print(f"Connected. Clock offset estimate={offset_ns} ns; uncertainty <=~{uncertainty_us} us")

            previous_profile = None
            def on_notify(_sender, data):
                nonlocal previous_profile
                received_ns = time.monotonic_ns()
                if len(data) != 13:
                    print(f"Ignoring unexpected {len(data)}-byte notification: {bytes(data).hex()}")
                    return
                profile, seq, request_us = struct.unpack("<BIQ", bytes(data))
                if previous_profile is not None and profile != previous_profile:
                    if 0 in seen[previous_profile]:
                        profile_closed.add(previous_profile)
                previous_profile = profile
                if seq == 0:
                    profile_start_seen.add(profile)
                seen[profile].add(seq)
                receive_esp_us = (received_ns - offset_ns) // 1000
                latency_us = receive_esp_us - request_us
                latencies[profile].append(latency_us)
                writer.writerow([received_ns, request_us, receive_esp_us,
                                 latency_us, uncertainty_us, profile, seq, bytes(data).hex()])
            await client.start_notify(CHAR_UUID, on_notify)
            print("Notify subscribed. Leave running through all 12 profiles (~120 s); Ctrl-C to stop.")
            stop_event = asyncio.Event()
            loop = asyncio.get_running_loop()
            loop.add_signal_handler(signal.SIGINT, stop_event.set)
            try:
                await stop_event.wait()
            finally:
                loop.remove_signal_handler(signal.SIGINT)
                await client.stop_notify(CHAR_UUID)

    with summary_path.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["profile", "received_unique", "internal_sequence_gaps", "inferred_missing_if_complete", "complete_profile", "latency_mean_us", "latency_median_us", "latency_p95_us", "latency_min_us", "latency_max_us", "clock_offset_uncertainty_us"])
        for profile in range(PROFILE_COUNT):
            seqs = seen[profile]
            gaps = max(seqs) - min(seqs) + 1 - len(seqs) if seqs else 0
            complete = profile in profile_start_seen and profile in profile_closed
            ls = latencies[profile]
            inferred_missing = SAMPLES_PER_PROFILE - len(seqs) if complete else ""
            if ls:
                ordered = sorted(ls)
                mean = f"{statistics.mean(ls):.2f}"
                median = f"{statistics.median(ls):.2f}"
                p95 = ordered[math.ceil(.95 * len(ordered)) - 1]
                minimum, maximum = min(ls), max(ls)
            else:
                mean = median = p95 = minimum = maximum = ""
            writer.writerow([profile, len(seqs), gaps, inferred_missing, int(complete), mean, median,
                             p95, minimum, maximum, uncertainty_us if offsets else ""])
    print(f"Saved notification data: {out}")
    print(f"Saved sequence summary: {summary_path}")

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        # asyncio cancels the coroutine; keep a concise user-facing exit message.
        print("Capture stopped")
