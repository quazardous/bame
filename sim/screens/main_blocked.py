"""
main_blocked.py – BAME main screen, "No Battery" state.

Mirrors the ``voltage < 1.0`` branch of ``updateDisplay()`` in src/main.cpp.

Run:
    python sim/render_screen.py sim/screens/main_blocked.py --format svg --out docs/screenshots/main_blocked.svg --scale 6
    python sim/render_screen.py sim/screens/main_blocked.py --format png --out docs/screenshots/main_blocked.png --scale 6
"""

import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from oled_svg import (
    OLED128x64Bicolor,
    SSD1306_WHITE, SSD1306_BLACK, SSD1306_INVERSE,
    SCREEN_W, SCREEN_H, YELLOW_H, BLUE_Y,
)
from bamegfx import BameGFX


def render(display):
    """Draw the 'No Battery' screen using BameGFX helpers."""
    display.clearDisplay()
    gfx = BameGFX(display)

    # --- Yellow zone: empty gauge (0%) -----------------------------------
    gfx.drawGauge(0)

    # --- Blue zone: "No Battery" message (size 2, centered-ish) ---------
    display.setTextSize(2)
    display.setCursor(4, BLUE_Y + 12)
    display.print("No Battery")

    display.display()


# ---------------------------------------------------------------------------
# Allow running directly for quick testing
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Render main_blocked screen")
    parser.add_argument("--format", choices=["svg", "png"], default="svg")
    parser.add_argument("--out", default=None)
    parser.add_argument("--scale", type=int, default=4)
    a = parser.parse_args()

    out = a.out or f"main_blocked.{a.format}"
    d = OLED128x64Bicolor()
    render(d)

    if a.format == "svg":
        d.write_svg(out, scale=a.scale)
    else:
        try:
            import cairosvg
        except ImportError:
            print("Error: PNG requires cairosvg.  Install with: pip install cairosvg")
            raise SystemExit(1)
        svg = d.to_svg(scale=a.scale)
        cairosvg.svg2png(bytestring=svg.encode(), write_to=out)
        print(f"PNG written to {out}")
