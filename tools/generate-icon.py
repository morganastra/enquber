#!/usr/bin/env python3
"""Generate enquber's placeholder icon: the top-left corner of a real QR code
encoding the string "enquber" (finder pattern, timing pattern, and the first
data modules), with a circular fade centered on the finder pattern so the
right and bottom edges dissolve to white. The fade is quantized per module:
every module is one flat gray, keeping the crisp pixel aesthetic.

Outputs (written to data/icon/ unless --output-dir is given):
  enquber.svg          scalable, per-module opacity   -> hicolor/scalable/apps
  enquber.png          512x512 raster                 -> hicolor/512x512/apps
  enquber.ico          16..256 px Windows icon        -> the exe resource
  enquber-welcome.bmp  164x314, welcome/finish pages  -> the NSIS installer
  enquber-header.bmp   150x57, inner page header      -> the NSIS installer
  msix/*.png           44..310 px tile logos          -> the MSIX package

Requires qrencode and ImageMagick (`magick`) on PATH.

    tools/generate-icon.py
    tools/generate-icon.py --size 256 --output-dir /tmp/icon
"""

from __future__ import annotations

import argparse
import math
import re
import shutil
import subprocess
import sys
from pathlib import Path

DEFAULT_OUT_DIR = Path(__file__).parent.parent / "data" / "icon"

SIZE = 16        # crop window, in QR modules
CX = CY = 3.5    # fade center: finder pattern center, in modules
R1 = 170 / 32    # hold radius: solid out to here (finder corner is at ~4.95)
R2 = 450 / 32    # fade-out radius: fully white beyond here
SAMPLES = 8      # sub-samples per module axis for area-averaging the fade
ICO_SIZES = (256, 128, 64, 48, 32, 24, 16)  # Windows icon frames, largest first
# The bitmap sizes NSIS' MUI pages expect, and the logo inside them.
WELCOME_BITMAP = (164, 314)
WELCOME_LOGO = 128
HEADER_BITMAP = (150, 57)
HEADER_LOGO = (40, 8)  # edge length, white margin around it
# The tile logos an MSIX package needs, as (file name, canvas, logo edge).
MSIX_ASSETS = (
    ("StoreLogo.png", (50, 50), 44),
    ("Square44x44Logo.png", (44, 44), 38),
    ("Square150x150Logo.png", (150, 150), 130),
    ("Square310x310Logo.png", (310, 310), 268),
    ("Wide310x150Logo.png", (310, 150), 130),
)
MSIX_DIR = "msix"


def fade(d: float) -> float:
    """Mask intensity at distance d from the fade center: 1 = solid black."""
    if d <= R1:
        return 1.0
    if d >= R2:
        return 0.0
    return (R2 - d) / (R2 - R1)


def module_intensity(mx: float, my: float) -> float:
    """Area-averaged fade over one module: 1.0 = black, 0.0 = white."""
    total = 0.0
    for sy in range(SAMPLES):
        for sx in range(SAMPLES):
            px = mx + (sx + 0.5) / SAMPLES
            py = my + (sy + 0.5) / SAMPLES
            total += fade(math.hypot(px - CX, py - CY))
    return total / (SAMPLES * SAMPLES)


def read_modules(text: str, size: int) -> set[tuple[int, int]]:
    """The black module coordinates in qrencode's SVG output for @p text."""
    svg = subprocess.run(
        ["qrencode", "-t", "SVG", "-m", "0", "-l", "M", "-o", "-", text],
        check=True, capture_output=True, text=True,
    ).stdout
    return {
        (int(x), int(y))
        for x, y in re.findall(
            r'<rect x="(\d+)" y="(\d+)" width="1" height="1" fill="#000000"', svg
        )
        if int(x) < size and int(y) < size
    }


def module_grid() -> dict[tuple[int, int], float]:
    return {
        (x, y): module_intensity(x, y) for x in range(SIZE) for y in range(SIZE)
    }


def svg_source(modules: set[tuple[int, int]],
               grid: dict[tuple[int, int], float]) -> str:
    # SVG: white background, one rect per black module with the fade as opacity.
    rects = "".join(
        f'  <rect x="{x}" y="{y}" width="1" height="1" fill="#000" '
        f'fill-opacity="{grid[(x, y)]:.2f}"/>\n'
        for x, y in sorted(modules)
        if grid[(x, y)] > 0.01
    )
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512" '
        f'viewBox="0 0 {SIZE} {SIZE}" shape-rendering="crispEdges">\n'
        f'  <rect width="{SIZE}" height="{SIZE}" fill="#fff"/>\n'
        f'{rects}</svg>\n'
    )


def pgm_source(modules: set[tuple[int, int]],
               grid: dict[tuple[int, int], float]) -> bytes:
    return (
        f"P5\n{SIZE} {SIZE}\n255\n".encode()
        + bytes(
            round(255 * (1 - grid[(x, y)])) if (x, y) in modules else 255
            for y in range(SIZE)
            for x in range(SIZE)
        )
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUT_DIR,
                        metavar="DIR",
                        help="directory to write enquber.svg and enquber.png "
                             "into (default: <repo>/data/icon)")
    parser.add_argument("--size", type=int, default=512, metavar="PX",
                        help="edge length of the raster PNG in pixels "
                             "(default: 512)")
    parser.add_argument("--quiet", action="store_true",
                        help="do not print the paths that were written")
    args = parser.parse_args(argv)

    missing = [tool for tool in ("qrencode", "magick") if not shutil.which(tool)]
    if missing:
        print(f"error: missing required tool(s): {', '.join(missing)}",
              file=sys.stderr)
        return 2
    if args.size < 1:
        parser.error("--size must be at least 1")

    modules = read_modules("enquber", SIZE)
    if not modules:
        print("error: qrencode produced no usable modules (unexpected SVG format)",
              file=sys.stderr)
        return 1
    grid = module_grid()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    svg_path = args.output_dir / "enquber.svg"
    png_path = args.output_dir / "enquber.png"
    ico_path = args.output_dir / "enquber.ico"
    svg_path.write_text(svg_source(modules, grid))

    # PNG: render the same grid as a 16x16 PGM, then scale up with point
    # sampling (nearest neighbor) so the modules stay perfectly crisp.
    subprocess.run(
        ["magick", "pgm:-", "-sample", f"{args.size}x{args.size}", "-strip",
         str(png_path)],
        input=pgm_source(modules, grid),
        check=True,
    )

    # ICO: the frames Windows asks for, from the same grid. The point filter
    # keeps the auto-resize from blurring the module edges.
    subprocess.run(
        ["magick", "pgm:-", "-filter", "point", "-define",
         f"icon:auto-resize={','.join(str(size) for size in ICO_SIZES)}",
         "-strip", str(ico_path)],
        input=pgm_source(modules, grid),
        check=True,
    )

    # The installer bitmaps: the logo centered on the tall welcome/finish page
    # strip, and right-aligned in the header of the inner pages. Both are plain
    # 24-bit BMPs on white, which is what the MUI pages expect.
    logo, margin = HEADER_LOGO
    welcome_path = args.output_dir / "enquber-welcome.bmp"
    header_path = args.output_dir / "enquber-header.bmp"
    subprocess.run(
        ["magick", str(png_path), "-sample", f"{WELCOME_LOGO}x{WELCOME_LOGO}",
         "-background", "white", "-gravity", "center",
         "-extent", f"{WELCOME_BITMAP[0]}x{WELCOME_BITMAP[1]}",
         "-type", "truecolor", "-define", "bmp:format=bmp3", "-strip",
         str(welcome_path)],
        check=True,
    )
    subprocess.run(
        ["magick", str(png_path), "-sample", f"{logo}x{logo}",
         "-bordercolor", "white", "-border", str(margin),
         "-background", "white", "-gravity", "east",
         "-extent", f"{HEADER_BITMAP[0]}x{HEADER_BITMAP[1]}",
         "-type", "truecolor", "-define", "bmp:format=bmp3", "-strip",
         str(header_path)],
        check=True,
    )

    # MSIX tile logos, on white like the other generated artwork.
    msix_dir = args.output_dir / MSIX_DIR
    msix_dir.mkdir(parents=True, exist_ok=True)
    for name, (width, height), logo_size in MSIX_ASSETS:
        subprocess.run(
            ["magick", str(png_path), "-sample", f"{logo_size}x{logo_size}",
             "-background", "white", "-gravity", "center",
             "-extent", f"{width}x{height}", "-define", "png:color-type=2",
             "-strip", str(msix_dir / name)],
            check=True,
        )

    if not args.quiet:
        print(f"wrote {svg_path}, {png_path}, {ico_path}, the installer bitmaps "
              f"and {len(MSIX_ASSETS)} MSIX logos in {msix_dir}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
