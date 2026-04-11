"""
main_discharging.py – BAME main screen, discharging state.

Mirrors ``updateDisplay()`` in src/main.cpp with current > 0.5 A (load).

Run:
    python sim/render_screen.py sim/screens/main_discharging.py --format svg --out docs/screenshots/main_discharging.svg --scale 6
    python sim/render_screen.py sim/screens/main_discharging.py --format png --out docs/screenshots/main_discharging.png --scale 6
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

# ---------------------------------------------------------------------------
# Sample data
# ---------------------------------------------------------------------------
VOLTAGE     = 13.2   # V
CURRENT     = 5.4    # A (positive = discharging)
POWER       = VOLTAGE * CURRENT
SOC_PERCENT = 72     # %
COULOMBS    = SOC_PERCENT * 80 * 3600 / 100  # approximate coulombs remaining
HOURS_LEFT  = (COULOMBS / 3600.0) / CURRENT
CAL_AH      = 2.3   # Ah accumulated in current calibration segment


def render(display):
    """Draw the main discharging screen using BameGFX helpers."""
    display.clearDisplay()
    gfx = BameGFX(display)

    # --- Yellow zone: SOC gauge ------------------------------------------
    gfx.drawGauge(SOC_PERCENT)

    # --- Blue zone -------------------------------------------------------
    # Line 1: Voltage (size 2) + remaining Ah right-aligned (size 2)
    display.setTextSize(2)
    display.setCursor(0, BLUE_Y + 2)
    display.print(f"{VOLTAGE:.1f}")
    display.setTextSize(1)
    display.print("V")

    ah_int = int(COULOMBS / 3600.0)
    ah_str = str(ah_int)
    ah_w = len(ah_str) * 12 + 12  # size-2 chars + "Ah"
    display.setTextSize(2)
    display.setCursor(SCREEN_W - ah_w, BLUE_Y + 2)
    display.print(ah_str)
    display.setTextSize(1)
    display.print("Ah")

    # Line 2: Power (left) + current right-aligned
    display.setTextSize(1)
    display.setCursor(0, BLUE_Y + 22)
    display.print(f"{int(abs(POWER))}W")

    i_str = f"{CURRENT:.1f}A"
    display.setCursor(SCREEN_W - len(i_str) * 6, BLUE_Y + 22)
    display.print(i_str)

    # Line 3: discharge triangle + time remaining + cal Ah
    ty = BLUE_Y + 37
    h = int(HOURS_LEFT)
    m = int((HOURS_LEFT - h) * 60)
    tbuf = f"{h:02d}:{m:02d}"

    # Left-pointing triangle = discharging (current > 0)
    display.fillTriangle(6, ty + 3, 0, ty, 0, ty + 6, SSD1306_WHITE)
    display.setCursor(10, ty)
    display.print(tbuf)

    # Cal Ah counter (right side)
    cal_str = f"{CAL_AH:.1f}Ah"
    display.setCursor(SCREEN_W - len(cal_str) * 6, ty)
    display.print(cal_str)

    display.display()


# ---------------------------------------------------------------------------
# Allow running directly for quick testing
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Render main_discharging screen")
    parser.add_argument("--format", choices=["svg", "png"], default="svg")
    parser.add_argument("--out", default=None)
    parser.add_argument("--scale", type=int, default=4)
    a = parser.parse_args()

    out = a.out or f"main_discharging.{a.format}"
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
