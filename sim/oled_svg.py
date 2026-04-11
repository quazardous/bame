"""
oled_svg.py – Python emulation of a bicolor SSD1306 128x64 OLED display.

Provides an API close to Adafruit_SSD1306 / Adafruit_GFX so that render
scripts can be written (or derived) almost line-for-line from the original
C++ firmware code.

Bicolor zones (matching BameGFX.h):
  y <  16  →  yellow  (#FFFF00)
  y >= 16  →  blue    (#3399FF)
  background → black  (#000000)

Usage example
-------------
    from oled_svg import OLED128x64Bicolor, SSD1306_WHITE, SSD1306_BLACK, SSD1306_INVERSE

    d = OLED128x64Bicolor()
    d.clearDisplay()
    d.setTextSize(1)
    d.setTextColor(SSD1306_WHITE)
    d.setCursor(0, 0)
    d.print("Hello BAME!")
    d.display()
    d.write_svg("out.svg", scale=4)
"""

# ---------------------------------------------------------------------------
# Adafruit-compatible color constants
# ---------------------------------------------------------------------------
SSD1306_WHITE   = 1
SSD1306_BLACK   = 0
SSD1306_INVERSE = 2

# ---------------------------------------------------------------------------
# Screen geometry (mirrors BameGFX.h)
# ---------------------------------------------------------------------------
SCREEN_W = 128
SCREEN_H = 64
YELLOW_H = 16   # rows 0-15  → yellow
BLUE_Y   = 16   # rows 16-63 → blue

# Adafruit built-in font dimensions (size 1 = 6×8 px, size n scales linearly)
FONT_W = 6
FONT_H = 8


# ---------------------------------------------------------------------------
# Color helpers
# ---------------------------------------------------------------------------
def _zone_color(y: int) -> str:
    """Return the hex fill color for a pixel at row y."""
    return "#FFFF00" if y < YELLOW_H else "#3399FF"


# ---------------------------------------------------------------------------
# Main display class
# ---------------------------------------------------------------------------
class OLED128x64Bicolor:
    """
    Emulates a bicolor SSD1306 128x64 OLED.

    Internal representation is a flat bytearray of SCREEN_W * SCREEN_H bytes
    where 0 = pixel off (black) and 1 = pixel on (color depends on y-zone).
    """

    def __init__(self):
        self._buf = bytearray(SCREEN_W * SCREEN_H)
        self._cursor_x = 0
        self._cursor_y = 0
        self._text_size = 1
        self._text_color = SSD1306_WHITE
        self._text_bg = SSD1306_BLACK

    # ------------------------------------------------------------------
    # Low-level pixel access
    # ------------------------------------------------------------------
    def _set_pixel(self, x: int, y: int, value: int) -> None:
        if 0 <= x < SCREEN_W and 0 <= y < SCREEN_H:
            self._buf[y * SCREEN_W + x] = value & 0x01

    def _get_pixel(self, x: int, y: int) -> int:
        if 0 <= x < SCREEN_W and 0 <= y < SCREEN_H:
            return self._buf[y * SCREEN_W + x]
        return 0

    # ------------------------------------------------------------------
    # Adafruit_SSD1306 / Adafruit_GFX compatible API
    # ------------------------------------------------------------------
    def clearDisplay(self) -> None:
        """Clear all pixels (set to off/black)."""
        for i in range(len(self._buf)):
            self._buf[i] = 0

    def setCursor(self, x: int, y: int) -> None:
        self._cursor_x = int(x)
        self._cursor_y = int(y)

    def setTextSize(self, size: int) -> None:
        self._text_size = max(1, int(size))

    def setTextColor(self, color: int, bg: int = None) -> None:
        self._text_color = color
        if bg is None:
            # Adafruit default: transparent background (we treat as BLACK)
            self._text_bg = SSD1306_BLACK
        else:
            self._text_bg = bg

    def display(self) -> None:
        """Commit the frame (no-op in simulation; exists for API compatibility)."""
        pass

    # ------------------------------------------------------------------
    # Primitives
    # ------------------------------------------------------------------
    def drawPixel(self, x: int, y: int, color: int = SSD1306_WHITE) -> None:
        if color == SSD1306_INVERSE:
            color = 1 - self._get_pixel(x, y)
        self._set_pixel(x, y, color)

    def drawRect(self, x: int, y: int, w: int, h: int,
                 color: int = SSD1306_WHITE) -> None:
        """Draw a 1-pixel wide rectangle outline."""
        for i in range(w):
            self.drawPixel(x + i, y, color)
            self.drawPixel(x + i, y + h - 1, color)
        for j in range(1, h - 1):
            self.drawPixel(x, y + j, color)
            self.drawPixel(x + w - 1, y + j, color)

    def fillRect(self, x: int, y: int, w: int, h: int,
                 color: int = SSD1306_WHITE) -> None:
        """Fill a rectangle."""
        for row in range(h):
            for col in range(w):
                px = x + col
                py = y + row
                if color == SSD1306_INVERSE:
                    self._set_pixel(px, py, 1 - self._get_pixel(px, py))
                else:
                    self._set_pixel(px, py, color)

    def drawLine(self, x0: int, y0: int, x1: int, y1: int,
                 color: int = SSD1306_WHITE) -> None:
        """Bresenham line."""
        dx = abs(x1 - x0)
        dy = abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx - dy
        while True:
            self.drawPixel(x0, y0, color)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 > -dy:
                err -= dy
                x0 += sx
            if e2 < dx:
                err += dx
                y0 += sy

    def drawFastHLine(self, x: int, y: int, w: int,
                      color: int = SSD1306_WHITE) -> None:
        """Fast horizontal line."""
        for i in range(w):
            self.drawPixel(x + i, y, color)

    def drawFastVLine(self, x: int, y: int, h: int,
                      color: int = SSD1306_WHITE) -> None:
        """Fast vertical line."""
        for i in range(h):
            self.drawPixel(x, y + i, color)

    def drawTriangle(self, x0: int, y0: int, x1: int, y1: int,
                     x2: int, y2: int, color: int = SSD1306_WHITE) -> None:
        """Draw a triangle outline."""
        self.drawLine(x0, y0, x1, y1, color)
        self.drawLine(x1, y1, x2, y2, color)
        self.drawLine(x2, y2, x0, y0, color)

    def fillTriangle(self, x0: int, y0: int, x1: int, y1: int,
                     x2: int, y2: int, color: int = SSD1306_WHITE) -> None:
        """Fill a triangle using scanline algorithm."""
        # Sort vertices by y
        verts = sorted([(x0, y0), (x1, y1), (x2, y2)], key=lambda v: v[1])
        (ax, ay), (bx, by), (cx, cy) = verts

        def interp(y, p0, p1):
            """Interpolate x at given y between two points."""
            (x0, y0), (x1, y1) = p0, p1
            if y1 == y0:
                return x0
            return x0 + (x1 - x0) * (y - y0) / (y1 - y0)

        for y in range(ay, cy + 1):
            if y < by:
                xa = interp(y, (ax, ay), (cx, cy))
                xb = interp(y, (ax, ay), (bx, by))
            elif y == by:
                xa = interp(y, (ax, ay), (cx, cy))
                xb = bx
            else:
                xa = interp(y, (ax, ay), (cx, cy))
                xb = interp(y, (bx, by), (cx, cy))
            xmin = int(min(xa, xb))
            xmax = int(max(xa, xb))
            for x in range(xmin, xmax + 1):
                self.drawPixel(x, y, color)

    # ------------------------------------------------------------------
    # Text rendering (monospace, Adafruit built-in font approximation)
    # ------------------------------------------------------------------
    # Minimal 5x7 bitmap font for printable ASCII (32-126).
    # Each character is encoded as 5 bytes, one per column (LSB = top row).
    # Source: Adafruit GFX glcdfont.c (MIT-licensed).
    _FONT5X7 = (
        b'\x00\x00\x00\x00\x00',  # 32 space
        b'\x00\x00\x5f\x00\x00',  # 33 !
        b'\x00\x07\x00\x07\x00',  # 34 "
        b'\x14\x7f\x14\x7f\x14',  # 35 #
        b'\x24\x2a\x7f\x2a\x12',  # 36 $
        b'\x23\x13\x08\x64\x62',  # 37 %
        b'\x36\x49\x55\x22\x50',  # 38 &
        b'\x00\x05\x03\x00\x00',  # 39 '
        b'\x00\x1c\x22\x41\x00',  # 40 (
        b'\x00\x41\x22\x1c\x00',  # 41 )
        b'\x14\x08\x3e\x08\x14',  # 42 *
        b'\x08\x08\x3e\x08\x08',  # 43 +
        b'\x00\x50\x30\x00\x00',  # 44 ,
        b'\x08\x08\x08\x08\x08',  # 45 -
        b'\x00\x60\x60\x00\x00',  # 46 .
        b'\x20\x10\x08\x04\x02',  # 47 /
        b'\x3e\x51\x49\x45\x3e',  # 48 0
        b'\x00\x42\x7f\x40\x00',  # 49 1
        b'\x42\x61\x51\x49\x46',  # 50 2
        b'\x21\x41\x45\x4b\x31',  # 51 3
        b'\x18\x14\x12\x7f\x10',  # 52 4
        b'\x27\x45\x45\x45\x39',  # 53 5
        b'\x3c\x4a\x49\x49\x30',  # 54 6
        b'\x01\x71\x09\x05\x03',  # 55 7
        b'\x36\x49\x49\x49\x36',  # 56 8
        b'\x06\x49\x49\x29\x1e',  # 57 9
        b'\x00\x36\x36\x00\x00',  # 58 :
        b'\x00\x56\x36\x00\x00',  # 59 ;
        b'\x08\x14\x22\x41\x00',  # 60 <
        b'\x14\x14\x14\x14\x14',  # 61 =
        b'\x00\x41\x22\x14\x08',  # 62 >
        b'\x02\x01\x51\x09\x06',  # 63 ?
        b'\x32\x49\x79\x41\x3e',  # 64 @
        b'\x7e\x11\x11\x11\x7e',  # 65 A
        b'\x7f\x49\x49\x49\x36',  # 66 B
        b'\x3e\x41\x41\x41\x22',  # 67 C
        b'\x7f\x41\x41\x22\x1c',  # 68 D
        b'\x7f\x49\x49\x49\x41',  # 69 E
        b'\x7f\x09\x09\x09\x01',  # 70 F
        b'\x3e\x41\x49\x49\x7a',  # 71 G
        b'\x7f\x08\x08\x08\x7f',  # 72 H
        b'\x00\x41\x7f\x41\x00',  # 73 I
        b'\x20\x40\x41\x3f\x01',  # 74 J
        b'\x7f\x08\x14\x22\x41',  # 75 K
        b'\x7f\x40\x40\x40\x40',  # 76 L
        b'\x7f\x02\x0c\x02\x7f',  # 77 M
        b'\x7f\x04\x08\x10\x7f',  # 78 N
        b'\x3e\x41\x41\x41\x3e',  # 79 O
        b'\x7f\x09\x09\x09\x06',  # 80 P
        b'\x3e\x41\x51\x21\x5e',  # 81 Q
        b'\x7f\x09\x19\x29\x46',  # 82 R
        b'\x46\x49\x49\x49\x31',  # 83 S
        b'\x01\x01\x7f\x01\x01',  # 84 T
        b'\x3f\x40\x40\x40\x3f',  # 85 U
        b'\x1f\x20\x40\x20\x1f',  # 86 V
        b'\x3f\x40\x38\x40\x3f',  # 87 W
        b'\x63\x14\x08\x14\x63',  # 88 X
        b'\x07\x08\x70\x08\x07',  # 89 Y
        b'\x61\x51\x49\x45\x43',  # 90 Z
        b'\x00\x7f\x41\x41\x00',  # 91 [
        b'\x02\x04\x08\x10\x20',  # 92 backslash
        b'\x00\x41\x41\x7f\x00',  # 93 ]
        b'\x04\x02\x01\x02\x04',  # 94 ^
        b'\x40\x40\x40\x40\x40',  # 95 _
        b'\x00\x01\x02\x04\x00',  # 96 `
        b'\x20\x54\x54\x54\x78',  # 97 a
        b'\x7f\x48\x44\x44\x38',  # 98 b
        b'\x38\x44\x44\x44\x20',  # 99 c
        b'\x38\x44\x44\x48\x7f',  # 100 d
        b'\x38\x54\x54\x54\x18',  # 101 e
        b'\x08\x7e\x09\x01\x02',  # 102 f
        b'\x0c\x52\x52\x52\x3e',  # 103 g
        b'\x7f\x08\x04\x04\x78',  # 104 h
        b'\x00\x44\x7d\x40\x00',  # 105 i
        b'\x20\x40\x44\x3d\x00',  # 106 j
        b'\x7f\x10\x28\x44\x00',  # 107 k
        b'\x00\x41\x7f\x40\x00',  # 108 l
        b'\x7c\x04\x18\x04\x78',  # 109 m
        b'\x7c\x08\x04\x04\x78',  # 110 n
        b'\x38\x44\x44\x44\x38',  # 111 o
        b'\x7c\x14\x14\x14\x08',  # 112 p
        b'\x08\x14\x14\x18\x7c',  # 113 q
        b'\x7c\x08\x04\x04\x08',  # 114 r
        b'\x48\x54\x54\x54\x20',  # 115 s
        b'\x04\x3f\x44\x40\x20',  # 116 t
        b'\x3c\x40\x40\x20\x7c',  # 117 u
        b'\x1c\x20\x40\x20\x1c',  # 118 v
        b'\x3c\x40\x30\x40\x3c',  # 119 w
        b'\x44\x28\x10\x28\x44',  # 120 x
        b'\x0c\x50\x50\x50\x3c',  # 121 y
        b'\x44\x64\x54\x4c\x44',  # 122 z
        b'\x00\x08\x36\x41\x00',  # 123 {
        b'\x00\x00\x7f\x00\x00',  # 124 |
        b'\x00\x41\x36\x08\x00',  # 125 }
        b'\x10\x08\x08\x10\x08',  # 126 ~
    )

    def _draw_char(self, x: int, y: int, ch: str, color: int, bg: int,
                   size: int) -> None:
        """Render a single character at pixel (x, y)."""
        code = ord(ch)
        if code < 32 or code > 126:
            return  # skip non-printable

        glyph = self._FONT5X7[code - 32]

        char_w = FONT_W * size   # including 1-col gap
        char_h = FONT_H * size

        for col in range(5):
            col_data = glyph[col]
            for row in range(7):
                bit = (col_data >> row) & 1
                pixel_color: int
                if color == SSD1306_INVERSE:
                    # For INVERSE text: we XOR over existing pixels
                    for dy in range(size):
                        for dx in range(size):
                            px = x + col * size + dx
                            py = y + row * size + dy
                            cur = self._get_pixel(px, py)
                            self._set_pixel(px, py, 1 - cur)
                    continue
                if bit:
                    pixel_color = color
                else:
                    pixel_color = bg
                for dy in range(size):
                    for dx in range(size):
                        px = x + col * size + dx
                        py = y + row * size + dy
                        self._set_pixel(px, py, pixel_color)

        # 1-column gap (col 5) filled with bg (skip for INVERSE)
        if color != SSD1306_INVERSE:
            for row in range(char_h):
                for dx in range(size):
                    self._set_pixel(x + 5 * size + dx, y + row, bg)

    def print(self, text) -> None:
        """Print text at the current cursor, advancing the cursor."""
        text = str(text)
        char_w = FONT_W * self._text_size
        char_h = FONT_H * self._text_size

        for ch in text:
            if ch == '\n':
                self._cursor_x = 0
                self._cursor_y += char_h
                continue
            if ch == '\r':
                self._cursor_x = 0
                continue

            # When using SSD1306_INVERSE, fill the character cell background
            # first with the zone color so the XOR produces visible inverse.
            if self._text_color == SSD1306_INVERSE:
                # Fill background cell with ON pixels so XOR produces black text
                self.fillRect(self._cursor_x, self._cursor_y,
                              char_w, char_h, SSD1306_WHITE)

            self._draw_char(self._cursor_x, self._cursor_y, ch,
                            self._text_color, self._text_bg,
                            self._text_size)
            self._cursor_x += char_w

    def println(self, text="") -> None:
        """Print text followed by a newline (Adafruit_GFX compatibility)."""
        self.print(str(text) + '\n')

    # ------------------------------------------------------------------
    # SVG export
    # ------------------------------------------------------------------
    def to_svg(self, scale: int = 4, background: str = "black") -> str:
        """
        Render the current frame to an SVG string.

        Parameters
        ----------
        scale : int
            Each OLED pixel maps to a scale×scale square.
        background : str
            SVG background color (default "black").

        Returns
        -------
        str
            Complete SVG document (can be written to .svg or inlined in Markdown).
        """
        w = SCREEN_W * scale
        h = SCREEN_H * scale

        lines = [
            f'<svg xmlns="http://www.w3.org/2000/svg" '
            f'width="{w}" height="{h}" '
            f'viewBox="0 0 {w} {h}">',
            f'  <rect width="{w}" height="{h}" fill="{background}"/>',
        ]

        # Collect runs of ON pixels to reduce element count
        for y in range(SCREEN_H):
            fill = _zone_color(y)
            run_start = None
            run_len = 0
            for x in range(SCREEN_W):
                if self._buf[y * SCREEN_W + x]:
                    if run_start is None:
                        run_start = x
                        run_len = 1
                    else:
                        run_len += 1
                else:
                    if run_start is not None:
                        rx = run_start * scale
                        ry = y * scale
                        rw = run_len * scale
                        rh = scale
                        lines.append(
                            f'  <rect x="{rx}" y="{ry}" '
                            f'width="{rw}" height="{rh}" fill="{fill}"/>'
                        )
                        run_start = None
                        run_len = 0
            # Flush tail run
            if run_start is not None:
                rx = run_start * scale
                ry = y * scale
                rw = run_len * scale
                rh = scale
                lines.append(
                    f'  <rect x="{rx}" y="{ry}" '
                    f'width="{rw}" height="{rh}" fill="{fill}"/>'
                )

        lines.append('</svg>')
        return '\n'.join(lines)

    def write_svg(self, path: str, scale: int = 4,
                  background: str = "black") -> None:
        """Write the SVG to a file."""
        svg = self.to_svg(scale=scale, background=background)
        with open(path, 'w', encoding='utf-8') as f:
            f.write(svg)
        print(f"SVG written to {path}")
