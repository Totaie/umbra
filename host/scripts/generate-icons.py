#!/usr/bin/env python3
"""Generate the Umbra Host icon set from the Aperture mark.

Kept identical to scripts/generate-icons.py in the Umbra client repo so the two
halves of Umbra can't drift apart visually. The mark is a U cut from a disc by a
shadow: a thick arc with rounded ends, defined geometrically rather than traced
from an SVG, so the vector and raster forms are guaranteed to agree.

Usage:
    pip install Pillow
    python scripts/generate-icons.py

Writes umbra.ico, umbra.svg and umbra.png at the repo root, plus a copy of the
.ico under the web assets, replacing Apollo's artwork.
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

SS = 16  # supersampling factor; downsampled with LANCZOS for clean 16px edges


def draw_mark(size, colour):
    """Render the mark at `size` px on a transparent square."""
    hi = size * SS
    img = Image.new("RGBA", (hi, hi), (0, 0, 0, 0))
    s = hi / GRID

    def box(x0, y0, x1, y1):
        return [x0 * s, y0 * s, x1 * s, y1 * s]

    # Bottom half of the annulus, clipped to y >= CY so the ring doesn't close over
    # the opening of the U.
    ring = Image.new("L", (hi, hi), 0)
    rd = ImageDraw.Draw(ring)
    rd.ellipse(box(CX - OUTER, CY - OUTER, CX + OUTER, CY + OUTER), fill=255)
    rd.ellipse(box(CX - INNER, CY - INNER, CX + INNER, CY + INNER), fill=0)
    rd.rectangle(box(0, 0, GRID, CY), fill=0)

    # The two uprights, with a disc at each end for the rounded cap
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
  <title>Umbra Host</title>
  <path d="M{LEFT:g} {TOP:g} L{LEFT:g} {CY:g} A{R:g} {R:g} 0 0 0 {RIGHT:g} {CY:g} L{RIGHT:g} {TOP:g}"
        fill="none"
        stroke="currentColor"
        stroke-width="{W:g}"
        stroke-linecap="round"
        stroke-linejoin="round"/>
</svg>
"""

ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]

# Silver, matching the client. The host's icon lives in the system tray and on a
# dark taskbar far more often than on a light background.
MARK_COLOUR = (201, 209, 224, 255)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    svg_path = os.path.join(root, "umbra.svg")
    with open(svg_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(SVG)
    print("wrote umbra.svg")

    frames = [draw_mark(n, MARK_COLOUR) for n in ICO_SIZES]

    ico_path = os.path.join(root, "umbra.ico")
    frames[-1].save(ico_path, format="ICO", sizes=[(n, n) for n in ICO_SIZES])
    print("wrote umbra.ico with", len(ICO_SIZES), "resolutions")

    # Used by the Linux packaging and the web interface
    png_path = os.path.join(root, "umbra.png")
    draw_mark(256, MARK_COLOUR).save(png_path, format="PNG")
    print("wrote umbra.png")

    web_dir = os.path.join(root, "src_assets", "common", "assets", "web", "public", "images")
    if os.path.isdir(web_dir):
        web_ico = os.path.join(web_dir, "umbra.ico")
        frames[-1].save(web_ico, format="ICO", sizes=[(n, n) for n in ICO_SIZES])
        print("wrote src_assets/.../images/umbra.ico")


if __name__ == "__main__":
    main()
