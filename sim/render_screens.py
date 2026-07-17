#!/usr/bin/env python3
"""
BaMe OLED screenshot renderer.

Generates PNG screenshots that replicate the firmware display output.
Targets the PROD build (no dev-only countdown, no cal counter).
Uses the Adafruit 5x7 font bitmap for pixel-accurate rendering.

Source of truth: updateDisplay() in src/display.cpp. Plug-and-forget build —
main screens only, no settings menu.

Usage:
    python sim/render_screens.py [--scale 6] [--outdir docs/screenshots]
"""

import argparse
import os
from PIL import Image, ImageDraw

# --- Display constants (match BameGFX.h) ---
W, H = 128, 64
YELLOW_H = 16
BLUE_Y = 16

# Firmware version (match BAME_VERSION in src/bame_state.h)
BAME_VERSION = "2.12"

# Colors for PNG
COL_BG = (0, 0, 0)
COL_YELLOW = (255, 220, 0)
COL_BLUE = (80, 180, 255)

# Adafruit GFX 5x7 font (same as oled_svg.py / firmware)
FONT = [
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
]


class Display:
    """Minimal SSD1306 framebuffer emulator."""

    def __init__(self):
        self.fb = [[0] * W for _ in range(H)]
        self.cx, self.cy = 0, 0
        self.size = 1

    def clear(self):
        self.fb = [[0] * W for _ in range(H)]

    def pixel(self, x, y, on=True):
        if 0 <= x < W and 0 <= y < H:
            self.fb[y][x] = 1 if on else 0

    def rect(self, x, y, w, h):
        for i in range(w):
            self.pixel(x + i, y)
            self.pixel(x + i, y + h - 1)
        for j in range(h):
            self.pixel(x, y + j)
            self.pixel(x + w - 1, y + j)

    def fill_rect(self, x, y, w, h, on=True):
        for j in range(h):
            for i in range(w):
                self.pixel(x + i, y + j, on)

    def fill_triangle(self, x0, y0, x1, y1, x2, y2):
        # Simple scanline fill
        pts = sorted([(x0, y0), (x1, y1), (x2, y2)], key=lambda p: p[1])
        def interp(ya, xa, yb, xb, y):
            if yb == ya: return xa
            return xa + (xb - xa) * (y - ya) // (yb - ya)
        for y in range(pts[0][1], pts[2][1] + 1):
            xs = []
            for a, b in [(pts[0], pts[1]), (pts[1], pts[2]), (pts[0], pts[2])]:
                if a[1] <= y <= b[1] or b[1] <= y <= a[1]:
                    xs.append(interp(a[1], a[0], b[1], b[0], y))
            if xs:
                x_min, x_max = min(xs), max(xs)
                for x in range(x_min, x_max + 1):
                    self.pixel(x, y)

    def text(self, x, y, s, size=1, inverse=False):
        for ch in str(s):
            code = ord(ch)
            if code < 32 or code > 126:
                continue
            glyph = FONT[code - 32]
            for col in range(5):
                for row in range(7):
                    bit = (glyph[col] >> row) & 1
                    on = (not bit) if inverse else bool(bit)
                    for dy in range(size):
                        for dx in range(size):
                            self.pixel(x + col * size + dx, y + row * size + dy, on)
            # Gap column
            for row in range(7 * size):
                for dx in range(size):
                    self.pixel(x + 5 * size + dx, y + row, inverse)
            x += 6 * size

    def text_right(self, x_right, y, s, size=1):
        x = x_right - len(str(s)) * 6 * size
        self.text(x, y, s, size)

    def to_png(self, path, scale=6):
        img = Image.new('RGB', (W * scale, H * scale), COL_BG)
        px = img.load()
        for y in range(H):
            color = COL_YELLOW if y < YELLOW_H else COL_BLUE
            for x in range(W):
                if self.fb[y][x]:
                    for dy in range(scale):
                        for dx in range(scale):
                            px[x * scale + dx, y * scale + dy] = color
        img.save(path)


# --- BameGFX helpers (match firmware) ---

def draw_gauge(d, percent, label=None):
    d.rect(0, 0, W, YELLOW_H)
    cols = int(percent * 126 / 100)
    if cols > 0:
        d.fill_rect(1, 1, cols, 14)
    text = label or f"{int(percent)}%"
    tw = len(text) * 12
    x = (W - tw) // 2
    d.text(x, 0, text, size=2, inverse=True)


def draw_charging_battery(d, x, y, full=False):
    # Mirror BameGFX::drawChargingBattery
    d.rect(x, y, 16, 10)
    d.fill_rect(x + 16, y + 2, 2, 6)
    d.fill_rect(x + 2, y + 2, 12 if full else 3, 6)


def draw_trend_arrow(d, x, y, direction):
    # direction: +1 up, -1 down, 0 none
    if direction > 0:
        d.fill_triangle(x, y + 6, x + 4, y, x + 8, y + 6)
    elif direction < 0:
        d.fill_triangle(x, y, x + 8, y, x + 4, y + 6)


# --- Main display: mirrors updateDisplay() in src/display.cpp (v2) ---

def _draw_main(d, voltage, current, soc, cap_ah, cells=4,
               soc_uncertain=False, capacity_learned=True, delivered_ah=0,
               slow_current=None):
    d.clear()
    remaining_ah = int(soc * cap_ah / 100)

    draw_gauge(d, soc)

    # Line 1: big Ah (left) + big voltage (right), '?' if soc uncertain
    ah_str = str(remaining_ah)
    d.text(0, BLUE_Y + 2, ah_str, size=2)
    ah_digits = len(ah_str)
    # Right of line 1: current, big, right-aligned (width varies with sign/decade)
    c_str = f"{current:.1f}"
    cur_left = W - 6 - len(c_str) * 12
    d.text(cur_left, BLUE_Y + 2, c_str, size=2)
    d.text(W - 6, BLUE_Y + 2, "A")
    d.text(ah_digits * 12, BLUE_Y + 2, "Ah")
    if soc_uncertain:
        d.text(ah_digits * 12, BLUE_Y + 10, "?")
    # Provisional capacity measured this cycle (blinks in firmware; shown here
    # in its visible phase): peak Ah delivered so far, next to the '?'.
    if not capacity_learned and delivered_ah >= 1:
        hint = str(int(delivered_ah + 0.5))
        hx = ah_digits * 12 + (8 if soc_uncertain else 0)
        if hx + len(hint) * 6 <= cur_left - 2:      # skip if the big current is in the way
            d.text(hx, BLUE_Y + 10, hint)

    # Line 2: power (smoothed) left, raw current right
    power = int(abs(voltage * current))
    d.text(0, BLUE_Y + 22, f"{power}W")
    d.text_right(W, BLUE_Y + 22, f"{voltage:.1f}V")

    # Line 3: HH:MM (active) or learned capacity (rest), '*' if not learned
    ty = BLUE_Y + 37
    active_i = 0.5  # ACTIVE_CURRENT
    if abs(current) > active_i:
        if current > 0:
            hours_left = remaining_ah / current
            d.fill_triangle(0, ty + 3, 6, ty, 6, ty + 6)  # discharge
        else:
            hours_left = (cap_ah - remaining_ah) / (-current)
            d.fill_triangle(6, ty + 3, 0, ty, 0, ty + 6)  # charge
        hours_left = max(0.0, min(99.9, hours_left))
        h = int(hours_left)
        m = int((hours_left - h) * 60)
        d.text(10, ty, f"{h:02d}:{m:02d}")
    else:
        d.text(0, ty, f"{int(cap_ah)}Ah")
        if not capacity_learned:
            d.text(28, ty, "*")

    # Bottom-right: same autonomy on the slow (τ ≈ 1 h) average, marked '~'.
    # The LOAD charging icon shares this spot and takes precedence.
    if voltage / cells >= 3.40 and abs(current) < 0.5:
        draw_charging_battery(d, 106, ty, full=True)
    elif slow_current is not None and slow_current > 0.1:
        hs = max(0.0, min(99.9, remaining_ah / slow_current))
        d.text(W - 36, ty, f"~{int(hs):02d}:{int((hs - int(hs)) * 60):02d}")


# --- Screens ---

def screen_main_discharge(d):
    # Capacity not yet confirmed: '?' + blinking provisional measurement (13 Ah
    # delivered so far) next to the big remaining Ah.
    _draw_main(d, voltage=13.2, current=3.7, soc=84, cap_ah=80,
               soc_uncertain=True, capacity_learned=False, delivered_ah=13,
               slow_current=1.2)


def screen_main_charge(d):
    # BUS install: charge current is visible as negative.
    _draw_main(d, voltage=14.1, current=-3.0, soc=89, cap_ah=80)


def screen_main_rest(d):
    _draw_main(d, voltage=13.42, current=0.0, soc=100, cap_ah=80)


def screen_no_battery(d):
    d.clear()
    draw_gauge(d, 0)
    d.text(4, BLUE_Y + 12, "No Battery", size=2)
    # Firmware version: fixed, small font, bottom-right (matches display.cpp).
    d.text_right(W, H - 8, f"v{BAME_VERSION}")


SCREENS = {
    'main_discharge': screen_main_discharge,
    'main_charge': screen_main_charge,
    'main_rest': screen_main_rest,
    'no_battery': screen_no_battery,
}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Generate BaMe OLED screenshots')
    parser.add_argument('--scale', type=int, default=6)
    parser.add_argument('--outdir', default='docs/screenshots')
    args = parser.parse_args()

    os.makedirs(args.outdir, exist_ok=True)

    for name, fn in SCREENS.items():
        d = Display()
        fn(d)
        path = os.path.join(args.outdir, f'{name}.png')
        d.to_png(path, scale=args.scale)
        print(f"  {path}")

    print(f"\n{len(SCREENS)} screenshots saved.")
