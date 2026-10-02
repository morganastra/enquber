#!/usr/bin/env python3
"""Order-balanced A/B of fully-settled startup between two commands.

tools/startup-settle.py measures each command in one long sequence, which is
fine for a single number but lets machine drift bias whichever command happens
to run first. This helper measures the baseline and the candidate (and an
optional reference) once per round and rotates the launch order each round, so a
slow period hits every command equally. It reports the per-command distributions
and the paired candidate-minus-baseline delta.

    tools/startup-ab.py --display :9 --runs 15 \\
        --baseline /tmp/enq-main/build/enquber --candidate ./build/enquber \\
        --reference kcalc

It reuses the capture and analysis from tools/startup-settle.py, so it needs
numpy and python-xlib like that tool.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import shlex
import statistics
import sys
from pathlib import Path

SETTLE = Path(__file__).with_name("startup-settle.py")


def load_settle():
    """Imports tools/startup-settle.py, whose name is not importable as-is."""
    spec = importlib.util.spec_from_file_location("startup_settle", SETTLE)
    module = importlib.util.module_from_spec(spec)
    # dataclasses looks the defining module up in sys.modules, so register it
    # before executing.
    sys.modules["startup_settle"] = module
    spec.loader.exec_module(module)
    return module


def percentile(values: list[float], fraction: float) -> float:
    """Nearest-rank percentile, matching tools/startup-settle.py."""
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(round(fraction * (len(ordered) - 1))))
    return ordered[index]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--display", default=os.environ.get("DISPLAY"),
                        help="X display to record (default: $DISPLAY)")
    parser.add_argument("--baseline", required=True,
                        help="command to compare against, e.g. a main build")
    parser.add_argument("--candidate", required=True,
                        help="command under test")
    parser.add_argument("--reference", default=None,
                        help="optional third command, e.g. kcalc")
    parser.add_argument("--runs", type=int, default=15,
                        help="order-balanced rounds (default: 15)")
    parser.add_argument("--duration", type=float, default=4.0,
                        help="seconds recorded after launch (default: 4)")
    parser.add_argument("--lead", type=float, default=0.2,
                        help="seconds recorded before launch (default: 0.2)")
    parser.add_argument("--fps", type=float, default=50.0,
                        help="capture rate (default: 50)")
    parser.add_argument("--json", action="store_true",
                        help="print the raw runs as JSON instead of a table")
    args = parser.parse_args(argv)

    if args.display is None:
        parser.error("no display: set DISPLAY or pass --display")
    if args.runs < 1:
        parser.error("--runs must be at least 1")
    for flag, text in (("--baseline", args.baseline), ("--candidate", args.candidate)):
        if not shlex.split(text):
            parser.error(f"{flag} must not be empty")

    settle = load_settle()
    commands = [
        ("baseline", shlex.split(args.baseline)),
        ("candidate", shlex.split(args.candidate)),
    ]
    if args.reference:
        commands.append(("reference", shlex.split(args.reference)))

    data: dict[str, list[float]] = {name: [] for name, _ in commands}
    paired: list[float] = []
    for round_index in range(args.runs):
        offset = round_index % len(commands)
        sequence = commands[offset:] + commands[:offset]
        values: dict[str, float] = {}
        for name, command in sequence:
            frames = settle.record(command, args.display, args.duration, args.lead,
                                   args.fps, False)
            result = settle.analyse(frames, 2.0, 0.05, 0.3)
            data[name].append(result.stable)
            values[name] = result.stable
        paired.append(values["candidate"] - values["baseline"])
        print(f"round {round_index + 1:>2}: "
              + "  ".join(f"{name} {values[name]:.3f}s" for name, _ in commands),
              file=sys.stderr)

    if args.json:
        print(json.dumps({"data": data, "paired_delta": paired}, indent=2))
        return 0

    print(f"startup A/B on {args.display} ({args.runs} order-balanced rounds, "
          "seconds)\n")
    print("| command | n | stable min | stable median | stable p90 | stable max |")
    print("| --- | ---: | ---: | ---: | ---: | ---: |")
    for name, _ in commands:
        values = data[name]
        print(f"| {name} | {len(values)} | {min(values):.3f} | "
              f"{statistics.median(values):.3f} | {percentile(values, 0.9):.3f} | "
              f"{max(values):.3f} |")

    print()
    faster = sum(1 for value in paired if value < 0)
    print(f"paired candidate - baseline delta: median {statistics.median(paired):+.3f}s "
          f"min {min(paired):+.3f}s max {max(paired):+.3f}s "
          f"({faster}/{len(paired)} rounds faster)")
    if args.reference:
        candidate_median = statistics.median(data["candidate"])
        reference_median = statistics.median(data["reference"])
        print(f"candidate median {candidate_median:.3f}s vs {args.reference} "
              f"{reference_median:.3f}s: delta {candidate_median - reference_median:+.3f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
