"""Summarize steady-state frame intervals from a timing or present CSV."""
import argparse
import csv
from itertools import chain
import math
from pathlib import Path
import statistics


def summarize(intervals, budget_ms):
    intervals = sorted(intervals)
    if not intervals:
        raise ValueError("No positive frame intervals in the selected window")

    def percentile(q):
        return intervals[math.ceil(q * len(intervals)) - 1]

    tail = intervals[-math.ceil(len(intervals) * 0.01):]
    return {
        "samples": len(intervals),
        "fps": 1000 / statistics.mean(intervals),
        "p50_ms": percentile(0.50),
        "p95_ms": percentile(0.95),
        "p99_ms": percentile(0.99),
        "max_ms": intervals[-1],
        "one_percent_low_fps": 1000 / statistics.mean(tail),
        "within_budget_pct": 100 * sum(value <= budget_ms for value in intervals) / len(intervals),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv_path", type=Path)
    parser.add_argument("--budget-ms", type=float, required=True)
    parser.add_argument("--skip-seconds", type=float, default=0)
    parser.add_argument("--duration", type=float)
    args = parser.parse_args()
    if args.budget_ms <= 0 or args.skip_seconds < 0 or (args.duration is not None and args.duration <= 0):
        parser.error("Use a positive budget and duration and a nonnegative warm-up")
    with args.csv_path.open(newline="", encoding="utf-8-sig") as source:
        rows = csv.DictReader(source)
        fields = set(rows.fieldnames or [])
        if {"interval_ms", "host_ms"} <= fields:
            interval, clock, divisor, label = "interval_ms", "host_ms", 1000, "CPU Present calls"
        elif {"frame_ms", "t_s"} <= fields:
            interval, clock, divisor, label = "frame_ms", "t_s", 1, "game timing trace"
        else:
            parser.error("Expected presents.csv or timing.csv")
        first = next(rows, None)
        if first is None:
            parser.error("The CSV is empty")
        origin = float(first[clock])
        values = []
        # The first Present interval has no preceding call; a game timing row does.
        for row in rows if interval == "interval_ms" else chain((first,), rows):
            elapsed = (float(row[clock]) - origin) / divisor
            value = float(row[interval])
            if elapsed < args.skip_seconds or (args.duration is not None and elapsed >= args.skip_seconds + args.duration):
                continue
            if math.isfinite(value) and value > 0:
                values.append(value)
    if not values:
        parser.error("No valid samples in the selected window")
    print(label)
    for key, value in summarize(values, args.budget_ms).items():
        print(f"{key}: {value:.3f}" if isinstance(value, float) else f"{key}: {value}")


if __name__ == "__main__":
    main()
