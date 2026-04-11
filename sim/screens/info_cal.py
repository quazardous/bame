"""
info_cal.py – Example OLED render script for the BAME "Info CAL" screen.

This screen mirrors the INFO_CAL display page from the BAME firmware
(src/main.cpp).  It demonstrates:

  • drawTitle-style header in the yellow zone (y < 16)
  • Multiple text rows in the blue zone (y >= 16)
  • A percentage gauge (fillRect + INVERSE text) in the yellow zone
  • drawRect for a simple horizontal bar

Run it with the render_screen CLI:

    python sim/render_screen.py sim/screens/info_cal.py --format svg --out out.svg
    python sim/render_screen.py sim/screens/info_cal.py --format png --out out.png

The render(display) function below is intentionally written in the same
style as the C++ firmware so that porting between the two is straightforward.
"""

import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from oled_svg import (
    OLED128x64Bicolor,
    SSD1306_WHITE, SSD1306_BLACK, SSD1306_INVERSE,
    SCREEN_W, SCREEN_H, YELLOW_H, BLUE_Y,
)

# ---------------------------------------------------------------------------
# Sample data (replace with live firmware values as needed)
# ---------------------------------------------------------------------------
VOLTAGE    = 13.2   # V
CURRENT    = -5.4   # A  (negative = charging)
SOC        = 72     # %
CAP_AH     = 82     # Ah (estimated)
NOM_AH     = 80     # Ah (nominal)
CELLS      = 4


def _draw_gauge(display, percent, label=None):
    """
    Replicate BameGFX::drawGauge() — fills the yellow zone with a bar
    and overlays centered text in INVERSE mode.
    """
    bx, by, bw, bh = 1, 1, 126, 14
    char_w_2 = 12  # 6px * size2

    # Border
    display.drawRect(0, 0, SCREEN_W, YELLOW_H, SSD1306_WHITE)

    # Filled portion
    cols = int(percent * bw / 100.0)
    cols = max(0, min(bw, cols))
    if cols > 0:
        display.fillRect(bx, by, cols, bh, SSD1306_WHITE)

    # Centered INVERSE label or percentage
    text = label if label else f"{int(percent)}%"
    w = len(text) * char_w_2
    x = (SCREEN_W - w) // 2
    display.setTextSize(2)
    display.setTextColor(SSD1306_INVERSE, SSD1306_INVERSE)
    display.setCursor(x, 0)
    display.print(text)


def _draw_title(display, title):
    """
    Replicate BameGFX::drawTitle() — centered, uppercase, size 1 in yellow zone.
    """
    t = title.upper()
    x = (SCREEN_W - len(t) * 6) // 2
    display.setTextSize(1)
    display.setTextColor(SSD1306_WHITE)
    display.setCursor(x, 4)
    display.print(t)


def render(display):
    """
    Draw the INFO CAL screen.

    This function is called by render_screen.py with a fresh
    OLED128x64Bicolor instance.
    """
    display.clearDisplay()

    # --- Yellow zone: SOC gauge ----------------------------------------
    _draw_gauge(display, SOC)

    # --- Blue zone: data rows ------------------------------------------
    display.setTextSize(1)
    display.setTextColor(SSD1306_WHITE)

    # Row 0: voltage + current
    v_str = f"{VOLTAGE:.1f}V"
    i_str = f"{CURRENT:+.1f}A"
    display.setCursor(0, BLUE_Y)
    display.print(v_str)
    display.setCursor(SCREEN_W - len(i_str) * 6, BLUE_Y)
    display.print(i_str)

    # Row 1: capacity nominal / estimated
    display.setCursor(0, BLUE_Y + 10)
    display.print(f"Cap:{CAP_AH}Ah nom:{NOM_AH}Ah")

    # Row 2: cells + SOC label
    display.setCursor(0, BLUE_Y + 20)
    display.print(f"{CELLS}S  SOC:{SOC}%")

    # Row 3: small horizontal energy bar (0–100% width of 126px)
    bar_y = BLUE_Y + 32
    display.drawRect(1, bar_y, 126, 8, SSD1306_WHITE)
    bar_fill = int(SOC * 124 / 100)
    if bar_fill > 0:
        display.fillRect(2, bar_y + 1, bar_fill, 6, SSD1306_WHITE)

    # Row 4: bar label
    display.setCursor(0, BLUE_Y + 42)
    display.print(f"CAL dSOC:{SOC}%")

    display.display()


# ---------------------------------------------------------------------------
# Allow running this script directly for quick testing
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    import argparse, pathlib

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
