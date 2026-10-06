#!/usr/bin/env python3
"""Measure how long an application's window takes to stop changing.

This records the Xvfb framebuffer across the whole launch and looks for the last
time the image changed. That is the point the interface has settled: menu entries,
spinners and late-loaded widgets are done moving. Measuring only when the window
appears misses this.

The screen is captured with XGetImage and compared a frame at a time, so the
result is a distribution over runs, not a single number. Several commands can be
measured together; the launch order rotates every round, so a slow stretch of the
machine hits each command equally instead of whichever happened to run next:

    tools/startup-settle.py --display :9 \
        --command ./build/enquber --command dolphin --command kcalc

To compare a build against a baseline and get the paired per-round delta (the
drift-resistant way to A/B a change), name them as a pair:

    tools/startup-settle.py --display :9 --runs 15 \
        --baseline /tmp/enq-main/build/enquber --candidate ./build/enquber

A command can carry a label with 'label,command', e.g.
--command 'kcalc,kcalc', so the summary and JSON name it usefully.

Pass --json to print every run (with the paired delta when a baseline is given);
tools/startup-stats.py turns that into a summary table and target verdict. Pass
--video DIR to also write one clip per run (ffmpeg, half resolution) so a result
can be watched back.

Needs numpy, python-xlib, and (only for --video) ffmpeg.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import shlex
import signal
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

try:
    import numpy as np
    from Xlib import X
    from Xlib import display as xdisplay
    from Xlib import error as xerror
except ImportError as error:
    sys.exit(f"startup-settle needs {error.name} (pip install numpy python-xlib)")


class Failure(Exception):
    pass


@dataclass
class Frame:
    t: float                    # seconds relative to launch (negative before it)
    gray: np.ndarray            # downsampled luminance, for the comparisons
    bgr: np.ndarray | None = None  # downsampled color, kept only for --video


@dataclass
class Run:
    command: str
    appear: float
    stable: float
    settle: float
    tail: float
    changes: int
    settled: bool
    clip: Path | None = None
    curve: list[tuple[float, float]] = field(default_factory=list)


def record(command: list[str], display: str, duration: float, lead: float,
           fps: float, want_video: bool) -> list[Frame]:
    """Records @p duration seconds from just before launching @p command."""
    d = xdisplay.Display(display)
    root = d.screen().root
    geometry = root.get_geometry()
    width, height = geometry.width, geometry.height
    interval = 1.0 / fps

    environment = dict(os.environ, DISPLAY=display)
    environment.pop("QT_QPA_PLATFORM", None)

    process: subprocess.Popen | None = None
    launch = 0.0
    started = time.monotonic()
    next_at = started
    frames: list[Frame] = []

    while True:
        now = time.monotonic()
        if process is None and now - started >= lead:
            launch = time.monotonic()
            process = subprocess.Popen(command, env=environment,
                                       stdout=subprocess.DEVNULL,
                                       stderr=subprocess.DEVNULL,
                                       start_new_session=True)
        if process is not None and now - launch >= duration:
            break

        raw = root.get_image(0, 0, width, height, X.ZPixmap, 0xffffffff).data
        stamp = time.monotonic()
        small = np.frombuffer(raw, dtype=np.uint8).reshape(height, width, 4)[::2, ::2, :3]
        gray = small.astype(np.float32).mean(axis=2)
        frames.append(Frame(stamp - launch if launch else stamp - started, gray,
                            small.copy() if want_video else None))

        next_at += interval
        delay = next_at - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        else:
            next_at = time.monotonic()

    if process is not None:
        try:
            os.killpg(os.getpgid(process.pid), signal.SIGTERM)
            process.wait(timeout=5)
        except (ProcessLookupError, subprocess.TimeoutExpired):
            with contextlib.suppress(ProcessLookupError):
                os.killpg(os.getpgid(process.pid), signal.SIGKILL)
            process.wait(timeout=5)

    d.close()
    return frames


def analyze(frames: list[Frame], appear_threshold: float, change_threshold: float,
            stable_window: float) -> Run:
    if len(frames) < 3:
        raise Failure("not enough frames were captured")

    # Diff consecutive downsampled frames once; everything below reuses it.
    diffs = [float(np.abs(frames[i].gray - frames[i - 1].gray).mean())
             for i in range(1, len(frames))]
    curve = [(frames[i].t, diffs[i - 1]) for i in range(1, len(frames))]

    # Ignore the lead-in for appearance. The window showing up is itself a large
    # consecutive difference, so no separate "before" reference is needed.
    appear_index = next((i for i in range(1, len(frames))
                         if frames[i].t >= 0 and diffs[i - 1] > appear_threshold), None)
    if appear_index is None:
        raise Failure("no window appeared during the clip (nothing changed enough)")

    change_indices = [i for i in range(appear_index, len(frames))
                      if diffs[i - 1] > change_threshold]
    last_change = change_indices[-1] if change_indices else appear_index

    appear = frames[appear_index].t
    stable = frames[last_change].t
    clip_end = frames[-1].t
    tail = clip_end - stable
    return Run(command="", appear=appear, stable=stable, settle=stable - appear,
               tail=tail, changes=max(0, len(change_indices) - 1),
               settled=tail >= stable_window, curve=curve)


def write_video(path: Path, frames: list[Frame], fps: float) -> None:
    if not frames or frames[0].bgr is None:
        return
    height, width = frames[0].bgr.shape[:2]
    # The captured frames are not evenly spaced, so use the rate that was asked
    # for. The clip is for watching, not for measuring.
    command = ["ffmpeg", "-y", "-loglevel", "error",
               "-f", "rawvideo", "-pix_fmt", "bgr24", "-s", f"{width}x{height}",
               "-r", f"{fps:.6g}", "-i", "-", "-an", "-pix_fmt", "yuv420p",
               str(path)]
    payload = b"".join(frame.bgr.tobytes() for frame in frames)
    subprocess.run(command, input=payload, check=True)


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    index = min(len(ordered) - 1, round(fraction * (len(ordered) - 1)))
    return ordered[index]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--display", default=os.environ.get("DISPLAY"),
                        help="X display to record; the whole root window is "
                             "captured, so use a throwaway Xvfb (default: "
                             "$DISPLAY; an error if neither is set)")
    parser.add_argument("--command", action="append", default=None,
                        help="a command to measure; repeat to compare (default: "
                             "./build/enquber). Quote it, e.g. --command 'konsole --hold'. "
                             "Prefix 'label,' (a single bare word) to name it, e.g. "
                             "--command 'kcalc,kcalc'. With --baseline/--candidate each "
                             "--command is measured as an extra 'reference' in the same "
                             "rounds")
    parser.add_argument("--baseline", default=None,
                        help="the command to compare against, reported as "
                             "'baseline' (or as 'label,command'). Pass with "
                             "--candidate to get the paired per-round delta "
                             "(default: none)")
    parser.add_argument("--candidate", default=None,
                        help="the command under test, reported as 'candidate' "
                             "(or as 'label,command'); measured with --baseline "
                             "(default: none)")
    parser.add_argument("--runs", type=int, default=3,
                        help="rounds to record; every command is measured once "
                             "per round and the launch order rotates each round "
                             "(default: 3)")
    parser.add_argument("--duration", type=float, default=4.0,
                        help="seconds recorded after launch (default: 4)")
    parser.add_argument("--lead", type=float, default=0.2,
                        help="seconds recorded before launch (default: 0.2)")
    parser.add_argument("--fps", type=float, default=50.0,
                        help="capture rate; the framebuffer can go far higher "
                             "(default: 50)")
    parser.add_argument("--appear-threshold", type=float, default=2.0,
                        help="mean per-frame pixel change (0-255) that counts as "
                             "the window appearing (default: 2.0)")
    parser.add_argument("--change-threshold", type=float, default=0.05,
                        help="mean per-frame pixel change that counts as still "
                             "moving; Xvfb draws exactly identical frames once "
                             "settled, so the default is deliberately tiny "
                             "(default: 0.05)")
    parser.add_argument("--stable-window", type=float, default=0.3,
                        help="quiet seconds required at the end of the clip for a "
                             "run to be called settled, i.e. tail >= this; it is "
                             "not a count of equal frames (default: 0.3)")
    parser.add_argument("--video", default=None,
                        help="directory for one .mp4 per measured run, named "
                             "<label>-<round>.mp4 (half resolution); needs ffmpeg, "
                             "and a failed write is only a warning")
    parser.add_argument("--json", action="store_true",
                        help="print the raw runs as JSON instead of a table; pipe "
                             "it into tools/startup-stats.py for a summary and "
                             "verdict")
    parser.add_argument("--verbose", action="store_true",
                        help="print the biggest changes of every run (ignored "
                             "with --json)")
    args = parser.parse_args(argv)

    if args.display is None:
        parser.error("no display: set DISPLAY or pass --display")
    if args.runs < 1:
        parser.error("--runs must be at least 1")

    def parse_spec(text: str, default_label: str = "") -> tuple[str, list[str]]:
        """Splits an optional 'label,command' spec; shlex then tokenises it.

        The separator is a comma rather than a semicolon because `just` splices
        *args back into a shell line and a semicolon would end the command. The
        text before the comma is only treated as a label when it is a single
        bare word (no spaces, no quotes), so a command that happens to contain a
        comma — `--command 'python3 -c "print(1,2)"'` — keeps working.
        """
        label, separator, body = text.partition(",")
        if separator and not label:
            # A leading comma carries no label: measure the body, not ",command".
            text = body
            separator = ""
        if separator and label == label.strip() and " " not in label and "\t" not in label:
            try:
                command = shlex.split(body)
            except ValueError:
                command = []
            if command:
                return (label, command)
        try:
            command = shlex.split(text)
        except ValueError as error:
            parser.error(f"could not parse command '{text}': {error}")
        if not command:
            parser.error(f"empty command in '{text}'")
        return ((default_label or " ".join(command)), command)

    # Commands are measured in rounds and the launch order is rotated each round,
    # so a period where the machine is slow hits every command equally. A single
    # command is just sequential runs; the pair (or more) get drift-resistant
    # medians instead of whichever happened to run last. The role is kept apart
    # from the display label so a custom label cannot hide the baseline/candidate
    # pair from the paired-delta calculation.
    specs: list[tuple[str, str, list[str]]] = []
    if args.baseline is not None or args.candidate is not None:
        if args.baseline is None or args.candidate is None:
            parser.error("--baseline and --candidate have to be given together")
        base_label, base_command = parse_spec(args.baseline, "baseline")
        cand_label, cand_command = parse_spec(args.candidate, "candidate")
        specs.append(("baseline", base_label, base_command))
        specs.append(("candidate", cand_label, cand_command))
        # Any --command on top is an extra reference measured in the same rounds.
        for text in args.command or []:
            label, command = parse_spec(text)
            specs.append(("reference", label, command))
    elif args.command:
        for text in args.command:
            label, command = parse_spec(text)
            specs.append((label, label, command))
    else:
        specs.append(("./build/enquber", "./build/enquber", ["./build/enquber"]))

    roles = {role for role, _, _ in specs}
    labels = {label for _, label, _ in specs}
    if "baseline" in roles and "candidate" in roles and len(labels) != len(specs):
        parser.error("commands must have distinct labels to be summarized apart")
    video_dir = Path(args.video) if args.video else None
    if video_dir:
        video_dir.mkdir(parents=True, exist_ok=True)

    results: dict[str, list[Run]] = {label: [] for _, label, _ in specs}
    paired: list[float] = []
    try:
        for round_index in range(args.runs):
            offset = round_index % len(specs)
            sequence = specs[offset:] + specs[:offset]
            stable_this_round: dict[str, float] = {}
            for role, label, command in sequence:
                try:
                    frames = record(command, args.display, args.duration, args.lead,
                                    args.fps, video_dir is not None)
                    result = analyze(frames, args.appear_threshold,
                                     args.change_threshold, args.stable_window)
                except (Failure, FileNotFoundError) as failure:
                    print(f"  {label}: {failure}", file=sys.stderr)
                    continue
                result.command = label
                if video_dir:
                    result.clip = video_dir / f"{label}-{round_index + 1}.mp4"
                    try:
                        write_video(result.clip, frames, args.fps)
                    except (subprocess.CalledProcessError, FileNotFoundError) as error:
                        print(f"     could not write {result.clip}: {error}",
                              file=sys.stderr)
                        result.clip = None
                results[label].append(result)
                stable_this_round[role] = result.stable
                print(f"  round {round_index + 1} {label}: appear {result.appear:.2f}s  "
                      f"stable {result.stable:.2f}s  settle {result.settle:.2f}s"
                      + (f"  -> {result.clip.name}" if result.clip else ""),
                      file=sys.stderr)
            if "baseline" in stable_this_round and "candidate" in stable_this_round:
                paired.append(stable_this_round["candidate"] - stable_this_round["baseline"])
    except (xerror.DisplayConnectionError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    if args.json:
        # Runs live under "runs" so metadata keys (paired_delta, pair) can never
        # collide with a command label, no matter what the user calls it.
        payload: dict[str, object] = {
            "runs": {label: [vars(run) | {"clip": str(run.clip) if run.clip else None}
                            for run in results[label]]
                     for _, label, _ in specs},
        }
        if paired:
            payload["paired_delta"] = paired
            payload["pair"] = {
                "baseline": next(label for role, label, _ in specs if role == "baseline"),
                "candidate": next(label for role, label, _ in specs if role == "candidate"),
            }
        print(json.dumps(payload, indent=2, default=str))
        return 0

    if args.verbose:
        for name, runs in results.items():
            for index, run in enumerate(runs, 1):
                top = sorted(run.curve, key=lambda item: item[1], reverse=True)[:6]
                marks = ", ".join(f"{t:+.2f}s:{value:.1f}" for t, value in top)
                print(f"  {name} #{index} largest changes: {marks}")

    print(f"\nstartup settle on {args.display} "
          f"({args.runs} rounds, {args.duration:g}s clip, {args.fps:g} fps, "
          f"change > {args.change_threshold:g}/255)")
    header = f"  {'command':<28} {'appear':>7} {'stable':>7} {'settle':>7} {'tail':>7}  status"
    print(header)
    for _, label, _ in specs:
        runs = results[label]
        if not runs:
            print(f"  {label:<28} no successful runs")
            continue
        appears = [run.appear for run in runs]
        stables = [run.stable for run in runs]
        settles = [run.settle for run in runs]
        tails = [run.tail for run in runs]
        status = "settled" if all(run.settled for run in runs) else "still moving at clip end"
        print(f"  {label:<28} {statistics.median(appears):6.2f}s "
              f"{statistics.median(stables):6.2f}s {statistics.median(settles):6.2f}s "
              f"{statistics.median(tails):6.2f}s  {status}")
        if len(runs) > 1:
            print(f"  {'':<28} stable min {min(stables):.2f}s  p90 "
                  f"{percentile(stables, 0.9):.2f}s  max {max(stables):.2f}s")
    if paired:
        faster = sum(1 for value in paired if value < 0)
        print(f"\npaired candidate - baseline delta: median {statistics.median(paired):+.3f}s "
              f"min {min(paired):+.3f}s max {max(paired):+.3f}s "
              f"({faster}/{len(paired)} rounds faster)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
