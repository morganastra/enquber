#!/usr/bin/env python3
"""Generate enquber's placeholder icon: the top-left corner of a real QR code
encoding the string "enquber" (finder pattern, timing pattern, and the first
data modules), with a circular fade centered on the finder pattern so the
right and bottom edges dissolve to white. The fade is quantized per module:
every module is one flat gray, keeping the crisp pixel aesthetic.

Outputs (written to data/icon/):
  enquber.svg  scalable, per-module opacity   -> hicolor/scalable/apps
  enquber.png  512x512 raster                 -> hicolor/512x512/apps

Requires: qrencode, ImageMagick (magick).
"""

import math
import re
import subprocess
import sys
import tempfile
from pathlib import Path

SIZE = 16        # crop window, in QR modules
CX = CY = 3.5    # fade center: finder pattern center, in modules
R1 = 170 / 32    # hold radius: solid out to here (finder corner is at ~4.95)
R2 = 450 / 32    # fade-out radius: fully white beyond here
SAMPLES = 8      # sub-samples per module axis for area-averaging the fade

out_dir = Path(__file__).parent.parent / "data" / "icon"

# The module matrix, from qrencode's SVG output (one 1x1 rect per module).
svg = subprocess.run(
    ["qrencode", "-t", "SVG", "-m", "0", "-l", "M", "-o", "-", "enquber"],
    check=True, capture_output=True, text=True,
).stdout
modules = {
    (int(x), int(y))
    for x, y in re.findall(
        r'<rect x="(\d+)" y="(\d+)" width="1" height="1" fill="#000000"', svg
    )
    if int(x) < SIZE and int(y) < SIZE
}


def fade(d):
    """Mask intensity at distance d from the fade center: 1 = solid black."""
    if d <= R1:
        return 1.0
    if d >= R2:
        return 0.0
    return (R2 - d) / (R2 - R1)


def module_intensity(mx, my):
    """Area-averaged fade over one module: 1.0 = black, 0.0 = white."""
    total = 0.0
    for sy in range(SAMPLES):
        for sx in range(SAMPLES):
            px = mx + (sx + 0.5) / SAMPLES
            py = my + (sy + 0.5) / SAMPLES
            total += fade(math.hypot(px - CX, py - CY))
    return total / (SAMPLES * SAMPLES)


grid = {
    (x, y): module_intensity(x, y) for x in range(SIZE) for y in range(SIZE)
}

# SVG: white background, one rect per black module with the fade as opacity.
rects = "".join(
    f'  <rect x="{x}" y="{y}" width="1" height="1" fill="#000" '
    f'fill-opacity="{grid[(x, y)]:.2f}"/>\n'
    for x, y in sorted(modules)
    if grid[(x, y)] > 0.01
)
(out_dir / "enquber.svg").write_text(
    f'<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512" '
    f'viewBox="0 0 {SIZE} {SIZE}" shape-rendering="crispEdges">\n'
    f'  <rect width="{SIZE}" height="{SIZE}" fill="#fff"/>\n'
    f'{rects}</svg>\n'
)

# PNG: render the same grid as a 16x16 PGM, then scale up with point
# sampling (nearest neighbor) so the modules stay perfectly crisp.
pgm = (
    f"P5\n{SIZE} {SIZE}\n255\n".encode()
    + bytes(
        round(255 * (1 - grid[(x, y)])) if (x, y) in modules else 255
        for y in range(SIZE)
        for x in range(SIZE)
    )
)
with tempfile.NamedTemporaryFile(suffix=".pgm", delete=False) as f:
    f.write(pgm)
    pgm_path = f.name
subprocess.run(
    ["magick", pgm_path, "-sample", "512x512", "-strip",
     str(out_dir / "enquber.png")],
    check=True,
)
Path(pgm_path).unlink()

print(f"wrote {out_dir}/enquber.svg and {out_dir}/enquber.png", file=sys.stderr)
