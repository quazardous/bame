"""
info_cal.py – BAME "Info CAL" screen render script.

Mirrors the ``batteryInfo(F("Info cal"))`` page in src/main.cpp.

Run:
    python sim/render_screen.py sim/screens/info_cal.py --format svg --out docs/screenshots/info_cal.svg --scale 6
    python sim/render_screen.py sim/screens/info_cal.py --format png --out docs/screenshots/info_cal.png --scale 6
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
# Sample data (representative values shown in the README)
# ---------------------------------------------------------------------------
TITLE         = "Info cal"
CAP_AH        = 82     # Ah estimated
NOM_AH        = 80     # Ah nominal
CAL_AH        = 12.3   # Ah accumulated in current calibration segment
CAL_TARGET_AH = 20.0   # Ah target for current calibration segment
DELTA_SOC     = 15     # % delta SOC of last completed cycle
VOLTAGE_START = 13.10  # V rest voltage at segment start
VOLTAGE_NOW   = 13.28  # V current voltage
SOC_PERCENT   = 72     # % state of charge


def render(display):
    """Draw the Info CAL screen using BameGFX helpers."""
    display.clearDisplay()
    gfx = BameGFX(display)

    # --- Yellow zone: title -----------------------------------------------
    gfx.drawTitle(TITLE)

    # --- Blue zone: calibration details -----------------------------------
    display.setTextSize(1)
    display.setTextColor(SSD1306_WHITE)

    # Line 1: Estimated capacity [nominal]   (matches firmware line 1)
    display.setCursor(0, BLUE_Y)
    display.print(f"Cap:{CAP_AH}Ah [{NOM_AH}]")

    # Line 2: Calibration Ah / target + delta%
    display.setCursor(0, BLUE_Y + 9)
    cal_str = f"{CAL_AH:.1f}/{CAL_TARGET_AH:.0f}Ah"
    display.print(cal_str)
    delta_str = f"d{DELTA_SOC}%"
    display.setCursor(SCREEN_W - len(delta_str) * 6, BLUE_Y + 9)
    display.print(delta_str)

    # Line 3: Start voltage → current voltage
    display.setCursor(0, BLUE_Y + 18)
    display.print(f"V:{VOLTAGE_START:.2f}->{VOLTAGE_NOW:.2f}V")

    # Line 4: SOC %
    display.setCursor(0, BLUE_Y + 27)
    display.print(f"SOC:{SOC_PERCENT}%")

    display.display()


# ---------------------------------------------------------------------------
# Allow running directly for quick testing
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Render info_cal screen")
    parser.add_argument("--format", choices=["svg", "png"], default="svg")
    parser.add_argument("--out", default=None)
    parser.add_argument("--scale", type=int, default=4)
    a = parser.parse_args()

    out = a.out or f"info_cal.{a.format}"
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
