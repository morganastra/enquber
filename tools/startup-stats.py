#!/usr/bin/env python3
"""Summarize startup-settle.py --json output.

Reads the JSON from a file or stdin and prints, per command, the median, min,
p90 and max of the "stable" time (launch -> last frame change), plus the
candidate-minus-reference delta and whether the median target is met. Only the
standard library is used, so it needs no numpy.

With a --baseline/--candidate pair it also prints the paired per-round delta
under the table; the candidate is then the "enquber" side of the verdict.

A piped run may be preceded by build logs (just(1) builds first and cmake writes
to stdout), so any leading lines before the JSON document are ignored.

    tools/startup-settle.py --display :9 --runs 12 --json \
        --command ./build/enquber --command 'kcalc,kcalc' \
        | tools/startup-stats.py --reference kcalc
    tools/startup-settle.py --display :9 --runs 15 --json \
        --baseline /tmp/enq-main/build/enquber --candidate ./build/enquber \
        | tools/startup-stats.py
    tools/startup-stats.py results.json --reference kcalc

The exit status is 0 when the candidate's median is within --threshold seconds
of the reference median, 1 when it is not, and 2 on an input error.
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
    index = min(len(ordered) - 1, round(fraction * (len(ordered) - 1)))
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


def numbers(runs, key: str, flat: bool = False) -> list[float]:
    """The values of @p key across @p runs; empty if the key is absent.

    @p runs is normally a list of run dicts; with @p flat it may also be a flat
    list of numbers, the shape used for the top-level paired delta. Values that
    are not numbers are treated as unusable rather than raising.
    """
    if not isinstance(runs, list):
        return []
    if flat:
        if all(isinstance(value, (int, float)) and not isinstance(value, bool)
               for value in runs):
            return [float(value) for value in runs]
        return []
    if not all(isinstance(run, dict) and key in run for run in runs):
        return []
    try:
        return [float(run[key]) for run in runs]
    except (TypeError, ValueError):
        return []


def load_json(text: str):
    """Parses @p text, tolerating leading build/log lines before the JSON."""
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        lines = text.splitlines()
        for index, line in enumerate(lines):
            if line.strip() != "{":
                continue
            try:
                return json.loads("\n".join(lines[index:]))
            except json.JSONDecodeError:
                continue
        raise


def median_cell(values: list[float]) -> str:
    return f"{statistics.median(values):.3f}" if values else "-"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("source", nargs="?", default="-",
                        help="JSON file from `startup-settle.py --json`; omit or "
                             "pass '-' to read stdin. Leading build/log lines "
                             "before the JSON are ignored (default: stdin)")
    parser.add_argument("--reference", default="kcalc",
                        help="label to compare the candidate against; matched by "
                             "basename then substring, and it must be present in "
                             "the input or the command exits 2 (default: kcalc)")
    parser.add_argument("--threshold", type=float, default=0.0,
                        help="allowed candidate-minus-reference median delta in "
                             "seconds: the target is met when the delta is <= "
                             "this. The default 0 means the candidate's median "
                             "must be no slower than the reference's; a negative "
                             "value requires it to be faster (default: 0)")
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

    if not isinstance(payload, dict):
        print("error: the input is not a JSON object", file=sys.stderr)
        return 2
    if not payload:
        print("error: no runs in the input", file=sys.stderr)
        return 2

    # startup-settle nests the per-command runs under "runs" so metadata keys
    # cannot collide with a label; older output put them at the top level.
    runs_by_label = payload.get("runs")
    if not isinstance(runs_by_label, dict):
        runs_by_label = {key: value for key, value in payload.items()
                         if isinstance(value, list) and key != "paired_delta"}

    paired_delta = payload.get("paired_delta")
    pair = payload.get("pair")
    if not isinstance(pair, dict):
        pair = None
    baseline_label = pair.get("baseline") if pair else None
    candidate_label = pair.get("candidate") if pair else None

    # Explicitly-named baseline/candidate come first; otherwise fall back to the
    # enquber-versus-reference convention used by the startup-settle recipe.
    labels = list(runs_by_label)
    if paired_delta and baseline_label in labels and candidate_label in labels:
        enquber = candidate_label
        other = [label for label in labels if label not in (baseline_label, candidate_label)]
        if args.reference in labels:
            reference = args.reference
        elif "reference" in labels:
            reference = "reference"
        elif len(other) == 1:
            reference = other[0]
        elif other:
            reference = find_command(other, args.reference)
        else:
            reference = baseline_label
    elif "baseline" in labels and "candidate" in labels:
        enquber = "candidate"
        if args.reference in labels:
            reference = args.reference
        elif "reference" in labels:
            reference = "reference"
        elif len(labels) > 2:
            other = [label for label in labels if label not in ("baseline", "candidate")]
            reference = other[0] if len(other) == 1 else find_command(other, args.reference)
        else:
            # With just a pair, compare the candidate against the baseline itself
            # and report the paired delta; the usual kcalc reference is optional.
            reference = "baseline"
    else:
        enquber = find_command(labels, "enquber")
        reference = find_command(labels, args.reference)
        if enquber is None:
            print(f"error: no enquber command in {labels}", file=sys.stderr)
            return 2

    stable: dict[str, list[float]] = {}
    appear: dict[str, list[float]] = {}
    settle: dict[str, list[float]] = {}
    counts: dict[str, int] = {}
    unsettled: dict[str, int] = {}
    for label, runs in runs_by_label.items():
        counts[label] = len(runs) if isinstance(runs, list) else 0
        unsettled[label] = (sum(1 for run in runs if isinstance(run, dict)
                                and run.get("settled") is False)
                            if isinstance(runs, list) else 0)
        stable[label] = numbers(runs, "stable")
        appear[label] = numbers(runs, "appear")
        settle[label] = numbers(runs, "settle")

    empty = [label for label, values in stable.items() if not values]
    if stable.get(enquber) and empty:
        print(f"warning: no usable stable values for {', '.join(empty)}",
              file=sys.stderr)
    if not stable[enquber]:
        print(f"error: no usable stable values for {enquber}", file=sys.stderr)
        return 2
    if reference is not None and not stable.get(reference):
        print(f"error: no usable stable values for '{reference}'", file=sys.stderr)
        return 2

    candidate_unsettled = unsettled.get(enquber, 0)
    if candidate_unsettled:
        print(f"warning: {enquber} still moving at clip end in "
              f"{candidate_unsettled}/{counts[enquber]} runs; "
              "stable is a lower bound", file=sys.stderr)
    if reference is not None and unsettled.get(reference):
        print(f"warning: {reference} still moving at clip end in "
              f"{unsettled[reference]}/{counts[reference]} runs; "
              "stable is a lower bound", file=sys.stderr)

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
    title = f"startup stats ({reference or args.reference} reference, seconds)"
    print(f"{title}\n")
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
    met = delta <= args.threshold and not candidate_unsettled
    print(f"{enquber} stable median {enquber_median:.3f}s vs {reference} "
          f"{reference_median:.3f}s: delta {delta:+.3f}s")
    if paired_delta:
        deltas = numbers(paired_delta, "", flat=True)
        if deltas:
            faster = sum(1 for value in deltas if value < 0)
            print(f"paired {enquber} - baseline delta: median "
                  f"{statistics.median(deltas):+.3f}s ({faster}/{len(deltas)} rounds faster)")
        else:
            print("warning: the paired delta is not a list of numbers",
                  file=sys.stderr)
    print(f"target ({enquber.split()[0]} <= {reference}, "
          f"threshold {args.threshold:+.3f}s): "
          f"{'MET' if met else 'NOT MET'}"
          + (" (candidate still moving at clip end)" if candidate_unsettled else ""))
    return 0 if met else 1


if __name__ == "__main__":
    sys.exit(main())
