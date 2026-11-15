#!/usr/bin/env python3
"""Normalize old and summary-only ESP-IDF UART captures into CSV files."""
import csv
import math
import re
import statistics
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
raw = (ROOT / "results/uart-capture.log").read_bytes().replace(b"\x00", b"").decode("utf-8", "replace")
raw = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", raw)

header = ["event", "sequence", "scheduled_us", "actual_us", "latency_us", "phase", "elapsed_us", "count", "total_us", "mean_us", "p95_us", "max_us", "heap_current_bytes", "heap_peak_bytes", "allocator_alloc_count", "allocator_failed_count", "ble_status", "ble_sent_count", "ble_failed_count", "notify_subscribed", "condition", "objects", "allocated_bytes", "reclaimed_bytes", "survivors", "overflow_count", "detail"]
events = []
values = defaultdict(list)
gc_aggregate = defaultdict(lambda: {"count": 0, "total": 0, "max": 0})
window_aggregate = defaultdict(list)
object_summary = {}
object_p95 = {}
active_condition = "unlabeled"
central_by_profile = defaultdict(list)
central_records = []

def add(event, **fields):
    row = {k: "" for k in header}
    row.update(event=event, **fields)
    events.append([row[k] for k in header])

for line in raw.splitlines():
    line = line.strip()
    m = re.fullmatch(r"probe,(\d+),(-?\d+),(-?\d+),(-?\d+)", line)
    if m:
        seq, scheduled, actual, latency = map(int, m.groups())
        add("probe", sequence=seq, scheduled_us=scheduled, actual_us=actual, latency_us=latency)
        values["probe_latency"].append(latency)
        continue
    m = re.fullmatch(r"task,(\d+),(-?\d+),(-?\d+),(-?\d+)", line)
    if m:
        seq, scheduled, actual, latency = map(int, m.groups())
        add("task", sequence=seq, scheduled_us=scheduled, actual_us=actual, latency_us=latency)
        values["task_latency"].append(latency)
        continue
    m = re.fullmatch(r"(probe_lateness_us|task_lateness_us|lua_call_us),(\d+),(-?\d+),(-?\d+),(-?\d+),(-?\d+)", line)
    if m:
        metric, count, minimum, mean, p95, maximum = m.groups()
        vals = list(map(int, (count, minimum, mean, p95, maximum)))
        add("window_summary", condition=active_condition, count=vals[0], mean_us=vals[2], p95_us=vals[3], max_us=vals[4], detail=f"metric={metric};min={vals[1]}")
        window_aggregate[(active_condition, metric)].append(vals)
        continue
    m = re.fullmatch(r"heap,(\d+),(\d+),(\d+),(\d+)", line)
    if m:
        current, peak, allocs, failed = map(int, m.groups())
        add("heap", heap_current_bytes=current, heap_peak_bytes=peak, allocator_alloc_count=allocs, allocator_failed_count=failed)
        values["heap_current"].append(current); values["heap_peak"].append(peak); values["alloc_failed"].append(failed)
        continue
    m = re.fullmatch(r"heap_summary,(\d+),(\d+),(\d+),(\d+)", line)
    if m:
        current, peak, allocs, failed = map(int, m.groups())
        add("heap_summary", heap_current_bytes=current, heap_peak_bytes=peak, allocator_alloc_count=allocs, allocator_failed_count=failed)
        values["heap_current"].append(current); values["heap_peak"].append(peak); values["alloc_failed"].append(failed)
        continue
    m = re.fullmatch(r"gc_phase,(elapsed_us)", line)
    if m:
        continue
    m = re.fullmatch(r"(pause|step|mark|atomic|sweep_string|sweep|finalize),(-?\d+)", line)
    if m:
        phase, elapsed = m.group(1), int(m.group(2))
        add("gc_phase", phase=phase, elapsed_us=elapsed)
        values["gc_" + phase].append(elapsed)
        continue
    m = re.fullmatch(r"gc_summary,([a-z_]+),(\d+),(\d+),(\d+),(\d+)", line)
    if m:
        phase, count, total, mean, maximum = m.groups()
        count, total, mean, maximum = map(int, (count, total, mean, maximum))
        add("gc_summary", phase=phase, count=count, total_us=total, mean_us=mean, max_us=maximum)
        a = gc_aggregate[(active_condition, phase)]; a["count"] += count; a["total"] += total; a["max"] = max(a["max"], maximum)
        continue
    m = re.fullmatch(r"object_case,([a-z0-9_]+),(\d+),(\d+),(\d+),(\d+),(-?\d+)", line)
    if m:
        name, objects, allocated, reclaimed, elapsed, survivors = m.groups()
        add("object_case", condition=name, objects=int(objects), allocated_bytes=int(allocated), reclaimed_bytes=int(reclaimed), elapsed_us=int(elapsed), survivors=int(survivors))
        values["object_gc_" + name].append(int(elapsed)); values["object_reclaimed_" + name].append(int(reclaimed))
        if int(survivors) >= 0: values["object_survivors_" + name].append(int(survivors))
        continue
    m = re.fullmatch(r"object_case_summary,([a-z0-9_]+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(-?\d+)", line)
    if m:
        name, trials, alloc_mean, alloc_max, free_mean, free_max, gc_mean, gc_max, survivors = m.groups()
        trials, alloc_mean, alloc_max, free_mean, free_max, gc_mean, gc_max, survivors = map(int, (trials, alloc_mean, alloc_max, free_mean, free_max, gc_mean, gc_max, survivors))
        add("object_case_summary", condition=name, count=trials, allocated_bytes=alloc_mean, reclaimed_bytes=free_mean, elapsed_us=gc_mean, max_us=gc_max, survivors=survivors, detail=f"allocated_max={alloc_max}; reclaimed_max={free_max}")
        values["object_gc_" + name].append(gc_mean)
        values["object_gc_max_" + name].append(gc_max)
        values["object_reclaimed_" + name].append(free_mean)
        values["object_allocated_" + name].append(alloc_mean)
        object_summary[name] = {"trials": trials, "alloc_mean": alloc_mean, "alloc_max": alloc_max,
                                "free_mean": free_mean, "free_max": free_max,
                                "gc_mean": gc_mean, "gc_max": gc_max, "survivors": survivors}
        if survivors >= 0: values["object_survivors_" + name].append(survivors)
        continue
    m = re.fullmatch(r"object_gc_p95,([a-z0-9_]+),(\d+)", line)
    if m:
        name, value = m.group(1), int(m.group(2))
        add("object_gc_p95", phase=name, elapsed_us=value)
        values["object_gc_p95_" + name].append(value)
        object_p95[name] = value
        continue
    m = re.fullmatch(r"condition,([a-z0-9_]+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)", line)
    if m:
        name, lua_core, probe_core, load, pause, stepmul, forced = m.groups()
        active_condition = name
        add("condition", condition=name, detail=f"lua_core={lua_core};probe_core={probe_core};load={load};pause={pause};stepmul={stepmul};forced_full_gc={forced}")
        continue
    m = re.fullmatch(r"probe_condition,([a-z0-9_]+)", line)
    if m:
        add("probe_condition", condition=m.group(1)); continue
    m = re.fullmatch(r"ble,(\d+),(-?\d+),(-?\d+),(-?\d+),(\d+),(\d+)", line)
    if m:
        seq, scheduled, requested, status, sent, failed = map(int, m.groups())
        add("ble_request", sequence=seq, scheduled_us=scheduled, actual_us=requested, ble_status=status, ble_sent_count=sent, ble_failed_count=failed)
        values["ble_status"].append(status)
        continue
    m = re.fullmatch(r"ble_summary,(\d+),(\d+),(\d+),(\d+)", line)
    if m:
        processed, sent, failed, subscribed = map(int, m.groups())
        add("ble_summary", count=processed, ble_sent_count=sent, ble_failed_count=failed, notify_subscribed=subscribed)
        values["ble_processed"].append(processed); values["ble_sent"].append(sent); values["ble_failed"].append(failed)
        continue
    m = re.fullmatch(r"#overflow,(\d+)", line)
    if m:
        count = int(m.group(1)); add("gc_probe_overflow", overflow_count=count); values["overflow"].append(count)

central_path = ROOT / "results/ble-central.csv"
if central_path.exists():
    with central_path.open(newline="") as f:
        for row in csv.DictReader(f):
            try:
                profile = int(row["profile"])
                seq = int(row["sequence"])
                request_us = int(row["esp_request_us"])
                receive_us = int(row["estimated_receive_esp_us"])
                latency_us = int(row["ble_latency_us"])
                uncertainty_us = int(row["clock_offset_uncertainty_us"])
            except (KeyError, TypeError, ValueError):
                continue
            central_by_profile[profile].append(latency_us)
            central_records.append((profile, seq, request_us, receive_us, latency_us, uncertainty_us))
            add("ble_central_receive", sequence=seq, actual_us=receive_us, latency_us=latency_us,
                condition=f"profile_{profile}", scheduled_us=request_us,
                detail=f"clock_offset_uncertainty_us={uncertainty_us}")

with (ROOT / "results/uart-events.csv").open("w", newline="") as f:
    w = csv.writer(f); w.writerow(header); w.writerows(events)

summary_header = ["metric", "unit", "samples", "mean", "median", "p95", "min", "max", "quality", "notes"]
summary = []
def p95(xs): return sorted(xs)[max(0, math.ceil(.95 * len(xs)) - 1)]
def add_metric(name, unit, xs, quality, notes):
    if not xs: return
    summary.append([name, unit, len(xs), f"{statistics.mean(xs):.3f}", f"{statistics.median(xs):.3f}", f"{p95(xs):.3f}", min(xs), max(xs), quality, notes])

add_metric("probe_latency_reported", "us", values["probe_latency"], "offset-sensitive", "Older raw format; planned-time baseline was about 105 us offset")
add_metric("lua_task_start_latency", "us", values["task_latency"], "legacy_log_contaminated", "Legacy firmware printed every GC sample; do not attribute delay to GC")
for phase in ["pause", "step", "mark", "atomic", "sweep_string", "sweep", "finalize"]:
    quality = "legacy_buffer_overflow" if values["gc_" + phase] else "summary_only"
    add_metric("gc_" + phase, "us", values["gc_" + phase], quality, "Legacy per-event data; GC probe overflowed" if quality == "legacy_buffer_overflow" else "No raw samples in this capture")
for (condition, phase), a in gc_aggregate.items():
    summary.append([f"gc_{phase}@{condition}", "us", a["count"], f"{(a['total']/a['count'] if a['count'] else 0):.3f}", "", "", "", a["max"], "aggregated_no_p95", "Aggregated phase summaries; P95 cannot be reconstructed from count/total/max alone"])
for (condition, metric), windows in window_aggregate.items():
    total_n = sum(x[0] for x in windows)
    weighted_mean = sum(x[0] * x[2] for x in windows) / total_n if total_n else 0
    summary.append([f"{metric}@{condition}", "us", total_n, f"{weighted_mean:.3f}", "", f"window_p95_max={max(x[3] for x in windows)}", min(x[1] for x in windows), max(x[4] for x in windows), "window_aggregated", "P95 is maximum per-window P95, not a pooled percentile"])
for name, s in object_summary.items():
    q = "30_trials_single_boot"
    summary.append(["object_gc_" + name, "us", s["trials"], s["gc_mean"], "", object_p95.get(name, ""), "", s["gc_max"], q, "Forced-GC trials in one boot; repeat across boots for stronger inference"])
    summary.append(["object_allocated_" + name, "bytes", s["trials"], s["alloc_mean"], "", "", "", s["alloc_max"], q, "Allocator increase per trial; max shown separately"])
    summary.append(["object_reclaimed_" + name, "bytes", s["trials"], s["free_mean"], "", "", "", s["free_max"], q, "Allocator decrease during collection; max shown separately"])
    if s["survivors"] >= 0:
        summary.append(["object_survivors_" + name, "objects", s["trials"], s["survivors"], "", "", "", "", q, "Mean verified survivors per trial"])
add_metric("lua_allocator_current", "bytes", values["heap_current"], "observed", "Lua allocator tracked bytes")
add_metric("lua_allocator_peak", "bytes", values["heap_peak"], "observed", "Peak Lua allocator tracked bytes")
add_metric("lua_allocator_failed_allocations", "count", values["alloc_failed"], "observed", "Allocator failure counter")
add_metric("gc_probe_dropped_records_per_dump", "records", values["overflow"], "overflow", "Nonzero values indicate dropped probe rows")
add_metric("ble_notify_api_status", "code", values["ble_status"], "legacy_no_connection", "Older per-request records")
add_metric("ble_processed", "requests", values["ble_processed"], "summary", "Counts in each emitted summary window")
add_metric("ble_sent", "requests", values["ble_sent"], "summary", "Cumulative successful notify submissions per window")
add_metric("ble_failed", "requests", values["ble_failed"], "summary", "Cumulative failed notify submissions per window")

if central_records:
    all_latencies = [x[4] for x in central_records]
    uncertainty = max(x[5] for x in central_records)
    add_metric("ble_latency_estimated_all_profiles", "us", all_latencies,
               f"clock_uncertainty_{uncertainty}us", "Host/ESP timer offset estimated by GATT reads; uncertainty exceeds much of the estimated one-way latency")
    try:
        with (ROOT / "results/ble-central-summary.csv").open(newline="") as f:
            for row in csv.DictReader(f):
                profile = int(row["profile"])
                sample_count = len(central_by_profile.get(profile, []))
                if not sample_count:
                    continue
                quality = f"estimated_clock_uncertainty_{row['clock_offset_uncertainty_us']}us"
                notes = (f"complete_profile={row['complete_profile']}; inferred_missing={row['inferred_missing_if_complete']}; "
                         "one-way latency precision is limited by clock-offset uncertainty")
                summary.append([f"ble_latency_profile_{profile}", "us", sample_count,
                                row["latency_mean_us"], row["latency_median_us"], row["latency_p95_us"],
                                row["latency_min_us"], row["latency_max_us"], quality, notes])
                summary.append([f"ble_unique_packets_profile_{profile}", "packets", row["received_unique"],
                                "", "", "", "", row["received_unique"],
                                "complete" if row["complete_profile"] == "1" else "partial",
                                f"internal sequence gaps={row['internal_sequence_gaps']}; inferred missing={row['inferred_missing_if_complete']}"])
    except (OSError, KeyError, ValueError):
        pass

uart_measurement_events = [e for e in events if e[0] != "ble_central_receive"]
if not uart_measurement_events:
    log_path = ROOT / "results/uart-capture.log"
    binary_path = ROOT / "build/kyunghee_esp32.bin"
    stale = log_path.exists() and binary_path.exists() and log_path.stat().st_mtime < binary_path.stat().st_mtime
    panic_count = len(re.findall(r"A stack overflow in task main", raw))
    summary.append(["uart_capture_status", "status", 0, "", "", "", "", "", "stale_or_no_metrics",
                    f"No parseable measurement rows; log older than current binary={int(stale)}; stack_overflow_messages={panic_count}"])
with (ROOT / "results/esp32_lua_gc_results.csv").open("w", newline="") as f:
    w = csv.writer(f); w.writerow(summary_header); w.writerows(summary)
print(f"normalized {len(events)} events -> results/uart-events.csv")
print(f"wrote {len(summary)} summary rows -> results/esp32_lua_gc_results.csv")
