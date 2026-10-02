#!/usr/bin/env python3
"""Measure how long an application's window takes to stop changing.

This records the Xvfb framebuffer across the whole launch and looks for the last
time the image changed. That is the point the interface has settled: menu entries,
spinners and late-loaded widgets are done moving. Measuring only when the window
appears misses this.

The screen is captured with XGetImage and compared a frame at a time, so the
result is a distribution over runs, not a single number. Several commands can be
compared in one go:

    tools/startup-settle.py --display :9 \
        --command ./build/enquber --command dolphin --command kcalc

Pass --video DIR to also write one clip per run (ffmpeg, half resolution) so a
result can be watched back.

Needs numpy, python-xlib, and (only for --video) ffmpeg.
"""

from __future__ import annotations

import argparse
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
    from Xlib import X, display as xdisplay, error as xerror
except ImportError as error:
    sys.exit(f"startup-settle needs {error.name} (pip install numpy python-xlib)")


class Failure(Exception):
    pass


@dataclass
class Frame:
    t: float                    # seconds relative to launch (negative before it)
    gray: np.ndarray            # downsampled luminance, for the comparisons
    bgr: np.ndarray | None = None  # downsampled colour, kept only for --video


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
            try:
                os.killpg(os.getpgid(process.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=5)

    d.close()
    return frames


def analyse(frames: list[Frame], appear_threshold: float, change_threshold: float,
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
    index = min(len(ordered) - 1, int(round(fraction * (len(ordered) - 1))))
    return ordered[index]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--display", default=os.environ.get("DISPLAY"),
                        help="X display to record (default: $DISPLAY)")
    parser.add_argument("--command", action="append", default=None,
                        help="a command to measure; repeat to compare (default: "
                             "./build/enquber). Quote it, e.g. --command 'konsole --hold'")
    parser.add_argument("--runs", type=int, default=3,
                        help="recordings per command (default: 3)")
    parser.add_argument("--duration", type=float, default=4.0,
                        help="seconds recorded after launch (default: 4)")
    parser.add_argument("--lead", type=float, default=0.2,
                        help="seconds recorded before launch (default: 0.2)")
    parser.add_argument("--fps", type=float, default=50.0,
                        help="capture rate; the framebuffer can go far higher "
                             "(default: 50)")
    parser.add_argument("--appear-threshold", type=float, default=2.0,
                        help="mean pixel change (0-255) that counts as the window "
                             "appearing (default: 2.0)")
    parser.add_argument("--change-threshold", type=float, default=0.05,
                        help="mean pixel change that counts as still moving; Xvfb "
                             "draws exactly identical frames once settled, so the "
                             "default is deliberately tiny (default: 0.05)")
    parser.add_argument("--stable-window", type=float, default=0.3,
                        help="seconds of no change needed to call it settled "
                             "(default: 0.3)")
    parser.add_argument("--video", default=None,
                        help="directory for one .mp4 per run (half resolution)")
    parser.add_argument("--json", action="store_true",
                        help="print the raw runs as JSON instead of a table")
    parser.add_argument("--verbose", action="store_true",
                        help="print the biggest changes of every run")
    args = parser.parse_args(argv)

    if args.display is None:
        parser.error("no display: set DISPLAY or pass --display")
    if args.runs < 1:
        parser.error("--runs must be at least 1")

    commands = [shlex.split(text) for text in args.command] if args.command \
        else [["./build/enquber"]]
    video_dir = Path(args.video) if args.video else None
    if video_dir:
        video_dir.mkdir(parents=True, exist_ok=True)

    results: dict[str, list[Run]] = {}
    try:
        for command in commands:
            label = " ".join(command)
            runs: list[Run] = []
            try:
                for run in range(args.runs):
                    frames = record(command, args.display, args.duration, args.lead,
                                    args.fps, video_dir is not None)
                    result = analyse(frames, args.appear_threshold,
                                     args.change_threshold, args.stable_window)
                    result.command = label
                    if video_dir:
                        result.clip = video_dir / f"{Path(command[0]).name}-{run + 1}.mp4"
                        try:
                            write_video(result.clip, frames, args.fps)
                        except (subprocess.CalledProcessError, FileNotFoundError) as error:
                            print(f"     could not write {result.clip}: {error}",
                                  file=sys.stderr)
                            result.clip = None
                    runs.append(result)
                    print(f"     {label}: appear {result.appear:.2f}s  "
                          f"stable {result.stable:.2f}s  settle {result.settle:.2f}s"
                          + (f"  -> {result.clip.name}" if result.clip else ""),
                          file=sys.stderr)
            except (Failure, FileNotFoundError) as failure:
                print(f"  {label}: {failure}", file=sys.stderr)
                continue
            results[label] = runs
    except (xerror.DisplayConnectionError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps({label: [vars(run) | {"clip": str(run.clip) if run.clip else None}
                                  for run in runs]
                          for label, runs in results.items()}, indent=2, default=str))
        return 0

    if args.verbose:
        for label, runs in results.items():
            for index, run in enumerate(runs, 1):
                top = sorted(run.curve, key=lambda item: item[1], reverse=True)[:6]
                marks = ", ".join(f"{t:+.2f}s:{value:.1f}" for t, value in top)
                print(f"  {label} #{index} largest changes: {marks}")

    print(f"\nstartup settle on {args.display} "
          f"({args.runs} runs each, {args.duration:g}s clip, {args.fps:g} fps, "
          f"change > {args.change_threshold:g}/255)")
    header = f"  {'command':<28} {'appear':>7} {'stable':>7} {'settle':>7} {'tail':>7}  status"
    print(header)
    for label, runs in results.items():
        appears = [run.appear for run in runs]
        stables = [run.stable for run in runs]
        settles = [run.settle for run in runs]
        tails = [run.tail for run in runs]
        status = "settled" if all(run.settled for run in runs) else "still moving at clip end"
        print(f"  {label:<28} {statistics.median(appears):6.2f}s "
              f"{statistics.median(stables):6.2f}s {statistics.median(settles):6.2f}s "
              f"{statistics.median(tails):6.2f}s  {status}")
        if len(runs) > 1:
            print(f"  {'':<28} settle min {min(settles):.2f}s  p90 "
                  f"{percentile(settles, 0.9):.2f}s  max {max(settles):.2f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
