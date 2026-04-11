"""
settings_menu.py – BAME settings menu screen.

Mirrors ``settingsMenu()`` in src/main.cpp, showing all five menu items with
row 0 ("Capacity") selected.

Run:
    python sim/render_screen.py sim/screens/settings_menu.py --format svg --out docs/screenshots/settings_menu.svg --scale 6
    python sim/render_screen.py sim/screens/settings_menu.py --format png --out docs/screenshots/settings_menu.png --scale 6
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
# Sample data (representative values)
# ---------------------------------------------------------------------------
BAME_VERSION      = "1.8"
BATTERY_CAP_NOM   = 80    # Ah
CELL_COUNT        = 4     # S
ECO_MODE          = False
SELECTED_ROW      = 0     # Row 0 "Capacity" is highlighted


def render(display):
    """Draw the settings menu screen using BameGFX helpers."""
    display.clearDisplay()
    gfx = BameGFX(display)

    # --- Yellow zone: title ----------------------------------------------
    gfx.drawTitle(f"Bame v{BAME_VERSION}")

    # --- Blue zone: menu items -------------------------------------------
    # Row 0: Capacity
    gfx.drawMenuItem(0, ' ', "Capacity", f"{BATTERY_CAP_NOM}Ah",
                     selected=(SELECTED_ROW == 0))

    # Row 1: Cells
    gfx.drawMenuItem(1, ' ', "Cells", f"{CELL_COUNT}S",
                     selected=(SELECTED_ROW == 1))

    # Row 2: Eco mode
    gfx.drawMenuItem(2, ' ', "Eco mode", "ON" if ECO_MODE else "OFF",
                     selected=(SELECTED_ROW == 2))

    # Row 3: Info cal (sub-menu, prefix '>')
    gfx.drawMenuItem(3, '>', "Info cal", None,
                     selected=(SELECTED_ROW == 3))

    # Row 4: Reset ALL
    gfx.drawMenuItem(4, ' ', "Reset ALL", None,
                     selected=(SELECTED_ROW == 4))

    display.display()


# ---------------------------------------------------------------------------
# Allow running directly for quick testing
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Render settings_menu screen")
    parser.add_argument("--format", choices=["svg", "png"], default="svg")
    parser.add_argument("--out", default=None)
    parser.add_argument("--scale", type=int, default=4)
    a = parser.parse_args()

    out = a.out or f"settings_menu.{a.format}"
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
