#!/usr/bin/env python3
"""Generate Umbra's icon set from the Aperture mark.

The mark is a U cut from a disc by a shadow: a thick arc with rounded ends. It is
defined here geometrically rather than by tracing an SVG, so the raster icons and
app/res/umbra.svg are guaranteed to be the same shape.

Usage:
    pip install Pillow
    python scripts/generate-icons.py

Writes:
    app/res/umbra.svg   vector mark, used for the window icon and on Linux
    app/umbra.ico       multi-resolution Windows icon (16/20/24/32/40/48/64/96/128/256)
"""
import os
from PIL import Image, ImageDraw

# Geometry, in a 64x64 design grid. The stroke centreline runs down x=LEFT, around a
# semicircle of radius R centred on (CX, CY), and back up x=RIGHT.
GRID = 64.0
CX, CY = 32.0, 30.0
R = 16.0          # centreline radius of the bend
W = 11.0          # stroke width
TOP = 14.0        # y where the two uprights begin
HALF = W / 2.0

LEFT = CX - R
RIGHT = CX + R
OUTER = R + HALF
INNER = R - HALF

# Supersample, then downsample with a good filter. Cheaper than antialiasing by hand
# and gives clean edges at 16px, which is where this icon lives most of the time.
SS = 16


def draw_mark(size, colour):
    """Render the mark at `size` px on a transparent square."""
    hi = size * SS
    img = Image.new("RGBA", (hi, hi), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    s = hi / GRID

    def box(x0, y0, x1, y1):
        return [x0 * s, y0 * s, x1 * s, y1 * s]

    # Bottom half of the annulus: fill the outer disc, then punch the inner disc out.
    # Drawing the ring first and clipping to y >= CY keeps the uprights clean.
    ring = Image.new("L", (hi, hi), 0)
    rd = ImageDraw.Draw(ring)
    rd.ellipse(box(CX - OUTER, CY - OUTER, CX + OUTER, CY + OUTER), fill=255)
    rd.ellipse(box(CX - INNER, CY - INNER, CX + INNER, CY + INNER), fill=0)
    # Keep only the bottom half so the ring doesn't close over the opening
    rd.rectangle(box(0, 0, GRID, CY), fill=0)

    # The two uprights, and a disc at each end for the rounded cap
    arms = Image.new("L", (hi, hi), 0)
    ad = ImageDraw.Draw(arms)
    ad.rectangle(box(LEFT - HALF, TOP, LEFT + HALF, CY), fill=255)
    ad.rectangle(box(RIGHT - HALF, TOP, RIGHT + HALF, CY), fill=255)
    ad.ellipse(box(LEFT - HALF, TOP - HALF, LEFT + HALF, TOP + HALF), fill=255)
    ad.ellipse(box(RIGHT - HALF, TOP - HALF, RIGHT + HALF, TOP + HALF), fill=255)

    mask = Image.new("L", (hi, hi), 0)
    mask.paste(ring, (0, 0), ring)
    mask.paste(arms, (0, 0), arms)

    img.paste(colour, (0, 0), mask)
    return img.resize((size, size), Image.LANCZOS)


SVG = f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64">
  <title>Umbra</title>
  <path d="M{LEFT:g} {TOP:g} L{LEFT:g} {CY:g} A{R:g} {R:g} 0 0 0 {RIGHT:g} {CY:g} L{RIGHT:g} {TOP:g}"
        fill="none"
        stroke="currentColor"
        stroke-width="{W:g}"
        stroke-linecap="round"
        stroke-linejoin="round"/>
</svg>
"""

# Sizes Windows asks for across the shell, plus 256 for the installer and store listings
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]

# Silver, so the mark reads on the dark titlebar and taskbar it mostly lives on. Windows
# composites the icon over user-chosen backgrounds, and a near-black mark disappears on
# a dark taskbar far more often than a light one disappears on a light background.
MARK_COLOUR = (201, 209, 224, 255)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    svg_path = os.path.join(root, "app", "res", "umbra.svg")
    with open(svg_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(SVG)
    print("wrote", os.path.relpath(svg_path, root))

    frames = [draw_mark(n, MARK_COLOUR) for n in ICO_SIZES]
    ico_path = os.path.join(root, "app", "umbra.ico")
    frames[-1].save(ico_path, format="ICO", sizes=[(n, n) for n in ICO_SIZES])
    print("wrote", os.path.relpath(ico_path, root), "with", len(ICO_SIZES), "resolutions")


if __name__ == "__main__":
    main()
