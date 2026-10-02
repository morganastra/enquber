# Enquber startup performance

Status: optimization complete; fully-settled startup is faster than `kcalc`.

## Goal

Fully-settled startup — from process launch until the window has stopped
changing — should be no slower than a reference application (`kcalc`), under the
same conditions, without giving up theme flexibility (Enquber still resolves
icons and colours from the active platform theme).

Measuring "settled" rather than "window appeared" matters: a window can show up
quickly and then keep redrawing as late-loaded icons or widgets arrive. The
earlier investigation (`doc/improvements.txt`) found that failed theme icon
lookups alone accounted for roughly 95 ms of a ~300 ms start, so the point at
which the interface stops moving is the honest number.

## Methodology

- **Display:** a throwaway `Xvfb` at 1280x1024x24. Xvfb redraws only when
  something changes and then emits byte-identical frames, so "still changing" is
  unambiguous.
- **Tool:** `tools/startup-settle.py` records the whole framebuffer with
  `XGetImage` across the launch, diffs consecutive frames, and reports per run:
  - `appear` — first large change, i.e. the window showing up;
  - `stable` — time of the **last** frame change, absolute from launch;
  - `settle` — `stable - appear`;
  - `tail` — time from `stable` to the end of the clip.
- **Metric:** the median `stable` time, launch to fully settled. It is the value
  the app actually owes the user and it is insensitive to a single slow cold
  start.
- **Runs:** the original baseline uses n=12 per command; the optimized
  comparison below is an order-balanced n=15 A/B.
- **Why `settle` is 0 here:** the default change threshold (0.05/255) treats any
  visible difference as movement, and on Xvfb both Enquber and `kcalc` draw
  their window in a single captured frame. The first large change is therefore
  also the last change, `stable` coincides with `appear`, and `settle` reads
  0.000 s. `stable` is the meaningful figure.

`tools/startup-stats.py` turns `startup-settle.py --json` output into the
medians, spread and delta. It is stdlib-only and needs no numpy.

`tools/startup-ab.py` does the same for a pairwise comparison, but measures the
baseline and the candidate once per round and rotates the launch order each
round, so machine drift cannot bias whichever runs first.

## How to reproduce

Start a throwaway display, then compare against the default reference (`kcalc`):

    Xvfb :9 -screen 0 1280x1024x24 -nolisten tcp &
    DISPLAY=:9 just startup-compare

`just startup-compare` builds first, runs `--runs 12` per command (override with
`runs=N`) and forwards any extra arguments. The `--display` value comes from
`$DISPLAY`; pass `--display :N` as an extra argument to override it. The
reference defaults to `kcalc` (override with `reference=NAME`).

For a paste-ready summary, feed the JSON through the stats helper:

    DISPLAY=:9 just startup-compare --json | tools/startup-stats.py

For the same-session A/B between a `main` binary and the optimized one, build
`main` into its own tree and use the order-balanced harness:

    git worktree add /tmp/enq-main f5f4758
    (cd /tmp/enq-main && cmake --preset default && cmake --build --preset default)
    DISPLAY=:9 just startup-ab /tmp/enq-main/build/enquber ./build/enquber --reference kcalc

## Baseline results

n=12 per command, Xvfb 1280x1024x24. Times in seconds.

| command | stable min | stable median | stable p90 | stable max | settle |
| --- | ---: | ---: | ---: | ---: | ---: |
| `./build/enquber` | 0.223 | 0.236 | — | 0.270 | 0.000 |
| `kcalc` | 0.165 | 0.185 | — | 0.209 | 0.000 |

The original n=12 summary recorded only min, median and max, so `stable p90`
is shown as `—`; it was not invented. `tools/startup-stats.py` prints it for
future runs. From the order statistics it is bounded below by the median and
above by the max: 0.236–0.270 s for Enquber, 0.185–0.209 s for `kcalc`.

Target: Enquber's `stable` median must be less than or equal to `kcalc`'s.
Baseline delta is **+0.051 s** (Enquber slower), so the target is not met yet.

## Optimized results

Two changes were made:

1. `theme::icon()` probes one ubiquitous name (`edit-copy`) once per active icon
   theme. If the theme cannot resolve it, every requested name goes straight to
   the bundled glyphs instead of paying for a failed `QIcon::fromTheme()` each.
   A theme that *does* provide the probe still resolves each name individually,
   so real themes keep their own icons.
2. The two action icons that are never displayed (`m_quitAction`,
   `m_helpAction` — help is the custom-painted `HelpButton` and quit has no
   widget) are no longer resolved at all.

Bundled glyphs are also cached per glyph-and-colour, so palette changes no
longer re-scale and re-tint them.

To separate the change from machine drift, two binaries were built from the same
compiler and flags — `main` at `f5f4758` versus the optimized branch — and run
with `tools/startup-ab.py` on Xvfb 1280x1024x24. It measures all three commands
once per round and rotates the launch order each round, so a slow period hits
every command equally. n=15 rounds per command.

| command | n | stable min | stable median | stable p90 | stable max |
| --- | ---: | ---: | ---: | ---: | ---: |
| `main` (`f5f4758`) | 15 | 0.225 | 0.246 | 0.297 | 0.311 |
| optimized | 15 | 0.125 | **0.147** | 0.167 | 0.167 |
| `kcalc` | 15 | 0.144 | 0.185 | 0.212 | 0.213 |

Paired optimized − `main` delta: median **−0.100 s**, range −0.166 s to
−0.079 s. Every one of the 15 paired rounds was faster, and the optimized
distribution does not overlap the baseline (optimized max 0.167 s < baseline min
0.225 s).

**Target: met.** Optimized Enquber settles in a median 0.147 s versus `kcalc`'s
0.185 s — about 38 ms, or 20%, ahead, and roughly 40% faster than the baseline.

At the syscall level the change is exactly what was intended: a `strace` of one
launch on hicolor drops the filesystem calls touching `/icons/` from ~129k to
~10k, and probes 13 action names down to the single `edit-copy` probe.

Reproduce the target metric with:

    Xvfb :9 -screen 0 1280x1024x24 -nolisten tcp &
    DISPLAY=:9 just runs=15 startup-compare --json | tools/startup-stats.py

## Conclusion

The startup cost was not inherent Qt work but repeated failed icon-theme
lookups: on a session whose theme ships no action icons, every `QIcon::fromTheme`
call walked the theme paths and came back empty. Probing once and falling back to
the bundled glyphs for the whole set removes that, while a theme that does carry
the icons is still used name by name. Dropping the two never-displayed action
icons removes their lookups outright.

Fully-settled Enquber startup is now faster than `kcalc` (median 0.147 s versus
0.185 s), comfortably meeting the goal, with no reduction in theme flexibility:
real themes are still honoured and the fallback glyphs still follow light/dark
palette changes, both covered by the unit tests.
