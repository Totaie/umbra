#!/usr/bin/env python3
"""Generate the Umbra Host icon set from the Aperture mark.

The geometry is kept identical to scripts/generate-icons.py in the Umbra client
repo so the two halves of Umbra can't drift apart visually. The mark is a U cut
from a disc by a shadow: a thick arc with rounded ends, defined geometrically
rather than traced from an SVG, so the vector and raster forms are guaranteed to
agree.

Usage:
    pip install Pillow
    python scripts/generate-icons.py

Writes umbra.ico/.svg/.png at the repo root, and the full tray set (idle,
playing, pausing, locked) under both web asset trees, replacing Apollo's
artwork. Tray states are the same mark in different colours, which is the only
thing that reliably reads at 16px.
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


SVG_TEMPLATE = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64">
  <title>{title}</title>
  <path d="M{left:g} {top:g} L{left:g} {cy:g} A{r:g} {r:g} 0 0 0 {right:g} {cy:g} L{right:g} {top:g}"
        fill="none"
        stroke="{colour}"
        stroke-width="{w:g}"
        stroke-linecap="round"
        stroke-linejoin="round"/>
</svg>
"""


def svg_for(title, colour_css):
    return SVG_TEMPLATE.format(title=title, colour=colour_css, left=LEFT, top=TOP,
                               cy=CY, r=R, right=RIGHT, w=W)


ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]

# Tray states. Silver when idle, because the host's icon sits on a dark taskbar far
# more often than a light one; the others only have to be distinguishable from it
# at 16px, which colour does and glyph detail does not.
STATES = {
    "umbra":         ("Umbra Host",            (201, 209, 224, 255), "#C9D1E0"),
    "umbra-playing": ("Umbra Host - streaming", (126, 211, 145, 255), "#7ED391"),
    "umbra-pausing": ("Umbra Host - paused",    (232, 190, 106, 255), "#E8BE6A"),
    "umbra-locked":  ("Umbra Host - locked",    (226, 122, 122, 255), "#E27A7A"),
}

MARK_COLOUR = STATES["umbra"][1]

WEB_DIRS = [
    os.path.join("src_assets", "common", "assets", "web", "public", "images"),
    os.path.join("src_assets", "common", "assets", "web-legacy", "public", "images"),
]


def write_state(directory, stem, title, rgba, css):
    """Write the .svg, .ico and 16/45/1024 .png for one tray state."""
    with open(os.path.join(directory, stem + ".svg"), "w", encoding="utf-8", newline="\n") as f:
        f.write(svg_for(title, css))

    frames = [draw_mark(n, rgba) for n in ICO_SIZES]
    frames[-1].save(os.path.join(directory, stem + ".ico"),
                    format="ICO", sizes=[(n, n) for n in ICO_SIZES])

    # 1024 is what Apollo's artwork used, and the web UI relies on the -16 and -45
    # variants existing rather than scaling in CSS.
    for px, suffix in ((1024, ""), (45, "-45"), (16, "-16")):
        draw_mark(px, rgba).save(os.path.join(directory, stem + suffix + ".png"), format="PNG")


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    title, rgba, css = STATES["umbra"]

    with open(os.path.join(root, "umbra.svg"), "w", encoding="utf-8", newline="\n") as f:
        f.write(svg_for(title, css))
    print("wrote umbra.svg")

    frames = [draw_mark(n, rgba) for n in ICO_SIZES]
    frames[-1].save(os.path.join(root, "umbra.ico"),
                    format="ICO", sizes=[(n, n) for n in ICO_SIZES])
    print("wrote umbra.ico with", len(ICO_SIZES), "resolutions")

    # Used by the Linux packaging and as the web UI's largest source image
    draw_mark(256, rgba).save(os.path.join(root, "umbra.png"), format="PNG")
    print("wrote umbra.png")

    for rel in WEB_DIRS:
        directory = os.path.join(root, rel)
        if not os.path.isdir(directory):
            print("skipping missing", rel)
            continue
        for stem, (t, c, x) in STATES.items():
            write_state(directory, stem, t, c, x)
        # The web UI also asks for a "logo-" prefixed copy of the idle mark.
        write_state(directory, "logo-umbra", *STATES["umbra"])
        print("wrote tray set into", rel)


if __name__ == "__main__":
    main()
