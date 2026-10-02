#!/usr/bin/env python3
"""Summarise startup-settle.py --json output.

Reads the JSON from a file or stdin and prints, per command, the median, min,
p90 and max of the "stable" time (launch -> last frame change), plus the
enquber-minus-reference delta and whether the median target is met. Only the
standard library is used, so it needs no numpy.

A piped run may be preceded by build logs (just(1) builds first and cmake writes
to stdout), so any leading lines before the JSON document are ignored.

    tools/startup-settle.py --display :9 --runs 12 --json \
        --command ./build/enquber --command kcalc \
        | tools/startup-stats.py
    tools/startup-stats.py results.json --reference kcalc

The exit status is 0 when enquber's median is within --threshold seconds of the
reference median, 1 when it is not, and 2 on an input error.
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import sys


def percentile(values: list[float], fraction: float) -> float:
    """Nearest-rank percentile, matching tools/startup-settle.py."""
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(round(fraction * (len(ordered) - 1))))
    return ordered[index]


def find_command(labels: list[str], needle: str) -> str | None:
    """Returns the label whose executable best matches @p needle."""
    parts = needle.split()
    if not parts:
        return None
    base = os.path.basename(parts[0])
    for label in labels:
        if os.path.basename(label.split()[0]) == base:
            return label
    for label in labels:
        if needle in label:
            return label
    return None


def numbers(runs: list[dict], key: str) -> list[float]:
    """The values of @p key across @p runs; empty if the key is absent."""
    values = [run[key] for run in runs if key in run]
    if len(values) != len(runs):
        return []
    return [float(value) for value in values]


def load_json(text: str):
    """Parses @p text, tolerating leading build/log lines before the JSON."""
    try:
        return json.loads(text)
    except json.JSONDecodeError as error:
        lines = text.splitlines()
        for index, line in enumerate(lines):
            if line.strip() != "{":
                continue
            try:
                return json.loads("\n".join(lines[index:]))
            except json.JSONDecodeError:
                continue
        raise error


def median_cell(values: list[float]) -> str:
    return f"{statistics.median(values):.3f}" if values else "-"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source", nargs="?", default="-",
                        help="JSON file from startup-settle.py (default: stdin)")
    parser.add_argument("--reference", default="kcalc",
                        help="command to compare enquber against (default: kcalc)")
    parser.add_argument("--threshold", type=float, default=0.0,
                        help="allowed enquber-minus-reference median delta in "
                             "seconds; the target is met when the delta is <= "
                             "this (default: 0)")
    args = parser.parse_args(argv)

    try:
        if args.source == "-":
            payload = load_json(sys.stdin.read())
        else:
            with open(args.source, encoding="utf-8") as stream:
                payload = load_json(stream.read())
    except (OSError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    if not payload:
        print("error: no runs in the input", file=sys.stderr)
        return 2

    labels = list(payload)
    enquber = find_command(labels, "enquber")
    reference = find_command(labels, args.reference)
    if enquber is None:
        print(f"error: no enquber command in {labels}", file=sys.stderr)
        return 2

    stable: dict[str, list[float]] = {}
    appear: dict[str, list[float]] = {}
    settle: dict[str, list[float]] = {}
    counts: dict[str, int] = {}
    for label, runs in payload.items():
        counts[label] = len(runs)
        stable[label] = numbers(runs, "stable")
        appear[label] = numbers(runs, "appear")
        settle[label] = numbers(runs, "settle")

    if not stable[enquber]:
        print(f"error: no usable stable values for {enquber}", file=sys.stderr)
        return 2
    if reference is not None and not stable[reference]:
        print(f"error: no usable stable values for {args.reference}",
              file=sys.stderr)
        return 2

    has_appear = any(appear.values())
    has_settle = any(settle.values())

    header = ("| command | n | stable med | stable min | stable p90 | stable max |")
    rule = ("| --- | ---: | ---: | ---: | ---: | ---: |")
    if has_appear:
        header += " appear med |"
        rule += " ---: |"
    if has_settle:
        header += " settle med |"
        rule += " ---: |"
    print(f"startup stats ({args.reference} reference, seconds)\n")
    print(header)
    print(rule)
    for label in labels:
        values = stable[label]
        if not values:
            row = f"| {label} | {counts[label]} | - | - | - | - |"
            if has_appear:
                row += " - |"
            if has_settle:
                row += " - |"
            print(row)
            continue
        row = (f"| {label} | {counts[label]} | {statistics.median(values):.3f} | "
               f"{min(values):.3f} | {percentile(values, 0.9):.3f} | "
               f"{max(values):.3f} |")
        if has_appear:
            row += f" {median_cell(appear[label])} |"
        if has_settle:
            row += f" {median_cell(settle[label])} |"
        print(row)

    print()
    if reference is None:
        print(f"no reference '{args.reference}' in the input; "
              "cannot compute the delta", file=sys.stderr)
        return 2

    enquber_median = statistics.median(stable[enquber])
    reference_median = statistics.median(stable[reference])
    delta = enquber_median - reference_median
    met = delta <= args.threshold
    print(f"{enquber} stable median {enquber_median:.3f}s vs {reference} "
          f"{reference_median:.3f}s: delta {delta:+.3f}s")
    print(f"target ({enquber.split()[0]} <= {args.reference}, "
          f"threshold {args.threshold:+.3f}s): "
          f"{'MET' if met else 'NOT MET'}")
    return 0 if met else 1


if __name__ == "__main__":
    sys.exit(main())
