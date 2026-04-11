"""
bamegfx.py – Python port of lib/BameGFX/BameGFX.cpp

Provides a BameGFX class that wraps an OLED128x64Bicolor display and exposes
the same high-level UI helpers as the C++ firmware's BameGFX class:
  - drawTitle(title)            : centered title in yellow zone, size 1, uppercase
  - drawGauge(percent, label)   : filled bar + XOR text in yellow zone
  - drawMenuItem(row, prefix, label, value, selected, editing)
  - drawChargingBattery(x, y)   : battery icon (static, at level 2/4)
  - tick()                      : advance animation frame counter (no-op by default)
  - drawPercentXOR(percent, y, zone_x, zone_w) : centered % text in XOR

Usage
-----
    from oled_svg import OLED128x64Bicolor
    from bamegfx import BameGFX

    d = OLED128x64Bicolor()
    d.clearDisplay()
    gfx = BameGFX(d)
    gfx.drawGauge(72)
    d.display()
    d.write_svg("out.svg", scale=4)
"""

import sys
import os

# Allow importing oled_svg when running directly from the sim/ directory
_SIM_DIR = os.path.dirname(os.path.abspath(__file__))
if _SIM_DIR not in sys.path:
    sys.path.insert(0, _SIM_DIR)

from oled_svg import (
    SSD1306_WHITE, SSD1306_BLACK, SSD1306_INVERSE,
    SCREEN_W, SCREEN_H, YELLOW_H, BLUE_Y,
)

# Adafruit GFX built-in font: 6×8 per char, text size 2 → 12×16
_CHAR_W = 12  # 6 * 2
_CHAR_H = 16  # 8 * 2


class BameGFX:
    """
    High-level UI helper for the BAME bicolor OLED.

    Mirrors the C++ BameGFX class in lib/BameGFX/BameGFX.cpp so that render
    scripts can be written almost line-for-line from the firmware source.
    """

    def __init__(self, display, frame: int = 2):
        """
        Parameters
        ----------
        display : OLED128x64Bicolor
            The display emulator instance to draw on.
        frame : int
            Initial animation frame counter (default 2 → battery at level 2/4).
        """
        self._disp = display
        self._frame = frame

    # ------------------------------------------------------------------
    # Public API (mirrors BameGFX.h)
    # ------------------------------------------------------------------

    def tick(self) -> None:
        """Advance the animation frame counter (call once per loop)."""
        self._frame += 1

    def drawPercentXOR(self, percent: int, y: int,
                       zone_x: int = 0, zone_w: int = SCREEN_W) -> None:
        """
        Draw percent + "%" centered in a zone using XOR (INVERSE) blending.
        Mirrors BameGFX::drawPercentXOR().
        """
        percent = max(0, min(100, int(percent)))
        text = f"{percent}%"
        total_w = len(text) * _CHAR_W
        x = zone_x + (zone_w - total_w) // 2

        self._disp.setTextSize(2)
        self._disp.setTextColor(SSD1306_INVERSE, SSD1306_INVERSE)
        self._disp.setCursor(x, y)
        self._disp.print(text)

    def drawGauge(self, percent: float, label: str = None) -> None:
        """
        Draw a gauge bar filling the entire yellow zone (rows 0-15).

        The bar is drawn first (fillRect), then the text is overlaid in
        SSD1306_INVERSE mode so it XOR-blends with the bar, producing
        black-on-yellow or white-on-black text depending on fill level.

        Mirrors BameGFX::drawGauge().

        Parameters
        ----------
        percent : float
            Fill level 0-100.
        label : str, optional
            If given, display this text instead of the numeric percentage.
        """
        percent = max(0.0, min(100.0, float(percent)))

        # 1-px border around the yellow zone
        self._disp.drawRect(0, 0, SCREEN_W, YELLOW_H, SSD1306_WHITE)

        # Inner fill area: 126×14 px
        bx, by, bw, bh = 1, 1, 126, 14
        cols = int(percent * bw / 100.0)
        if cols > 0:
            self._disp.fillRect(bx, by, cols, bh, SSD1306_WHITE)

        # Overlay text in XOR/INVERSE mode
        if label is not None:
            w = len(str(label)) * _CHAR_W
            x = (SCREEN_W - w) // 2
            self._disp.setTextSize(2)
            self._disp.setTextColor(SSD1306_INVERSE, SSD1306_INVERSE)
            self._disp.setCursor(x, 0)
            self._disp.print(str(label))
        else:
            self.drawPercentXOR(int(percent), 0, 0, SCREEN_W)

    def drawTitle(self, title: str) -> None:
        """
        Draw a centered, uppercase title in the yellow zone (y=4, size 1).

        Mirrors BameGFX::drawTitle().
        """
        t = str(title).upper()
        x = (SCREEN_W - len(t) * 6) // 2
        self._disp.setTextSize(1)
        self._disp.setTextColor(SSD1306_WHITE)
        self._disp.setCursor(x, 4)
        self._disp.print(t)

    def drawMenuItem(self, row: int, prefix_char: str, label: str,
                     value: str = None, selected: bool = False,
                     editing: bool = False) -> None:
        """
        Draw one menu item in the blue zone.

        Layout (size 1 = 6×8 px per char, 8 px row height):
          y = BLUE_Y + row * 8
          Left : <prefix_char> <label>
          Right: <value>  or  [<value>] when editing

        Mirrors BameGFX::drawMenuItem().

        Parameters
        ----------
        row : int
            Zero-based row index (0-5 fit in the blue zone).
        prefix_char : str
            Single character prefix (e.g. '>' for sub-menu, ' ' otherwise).
        label : str
            Menu item label.
        value : str, optional
            Current value string (right-aligned).
        selected : bool
            If True, fill the row with white and draw text in black (inverse).
        editing : bool
            If True, wrap value in '[' ']' to indicate edit mode.
        """
        y = BLUE_Y + row * 8
        self._disp.setTextSize(1)

        if selected:
            self._disp.fillRect(0, y, SCREEN_W, 8, SSD1306_WHITE)
            self._disp.setTextColor(SSD1306_BLACK, SSD1306_WHITE)
        else:
            self._disp.setTextColor(SSD1306_WHITE, SSD1306_BLACK)

        self._disp.setCursor(0, y)
        self._disp.print(prefix_char)
        self._disp.print(' ')
        self._disp.print(str(label))

        if value is not None and str(value) != '':
            v = str(value)
            if editing:
                display_v = f'[{v}]'
            else:
                display_v = v
            vw = len(display_v) * 6
            self._disp.setCursor(SCREEN_W - vw, y)
            self._disp.print(display_v)

        # Reset text color to default white
        self._disp.setTextColor(SSD1306_WHITE)

    def drawChargingBattery(self, x: int, y: int) -> None:
        """
        Draw a small charging battery icon at (x, y).

        The body is 16×10 px with a 2×6 tip on the right.
        The fill level cycles through 5 states (0 = empty, 4 = full) driven by
        self._frame // 3.  In static renders (frame=2) this shows level 2/4.

        Mirrors BameGFX::drawChargingBattery().
        """
        bw, bh = 16, 10

        # Body outline
        self._disp.drawRect(x, y, bw, bh, SSD1306_WHITE)
        # Positive terminal tip (right side)
        self._disp.fillRect(x + bw, y + 2, 2, bh - 4, SSD1306_WHITE)

        # Animated fill
        level = (self._frame // 3) % 5   # 0-4
        fill_w = level * 3                # 0, 3, 6, 9, 12 px
        if fill_w > 0:
            self._disp.fillRect(x + 2, y + 2, fill_w, bh - 4, SSD1306_WHITE)
