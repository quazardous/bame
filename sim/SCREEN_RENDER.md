# BAME – OLED Screen Renderer

A lightweight Python tool that emulates the bicolor **SSD1306 128×64** OLED
display used in BAME and exports snapshots as **SVG** (zero dependencies) or
**PNG** (optional dependency: `cairosvg`).

The API is intentionally close to the Adafruit_SSD1306 / Adafruit_GFX API used
in the firmware, so render scripts can be written (or copy-pasted) almost
line-for-line from the C++ source.

---

## Files

| File | Purpose |
|------|---------|
| `sim/oled_svg.py` | Display emulation + SVG export |
| `sim/render_screen.py` | CLI runner |
| `sim/screens/info_cal.py` | Example render script |

---

## Bicolor conventions

The screen is divided into two colour zones, matching `lib/BameGFX/BameGFX.h`:

| Zone | Rows | Color |
|------|------|-------|
| Yellow | 0 – 15 (`YELLOW_H = 16`) | `#FFFF00` |
| Blue | 16 – 63 (`BLUE_Y = 16`) | `#3399FF` |
| Background | — | black |

Any pixel that is "ON" (value = 1) automatically gets the colour of its zone.
The calling code never has to specify yellow or blue explicitly.

---

## Installation

### SVG only (no extra dependencies)

The `oled_svg.py` module uses only the Python standard library.

```bash
# No installation needed – just run the renderer directly
python sim/render_screen.py sim/screens/info_cal.py --format svg --out out.svg
```

### PNG support (optional)

```bash
pip install cairosvg
```

Then use `--format png`:

```bash
python sim/render_screen.py sim/screens/info_cal.py --format png --out out.png
```

If `cairosvg` is not installed and you request `--format png`, the tool prints
a clear error message and exits.

---

## CLI reference

```
python sim/render_screen.py <script.py> [--format svg|png] [--out <path>] [--scale N]
```

| Option | Default | Description |
|--------|---------|-------------|
| `script` | *(required)* | Path to a render script |
| `--format` | `svg` | Output format: `svg` or `png` |
| `--out` | `<script_name>.<format>` | Output file path |
| `--scale` | `4` | Pixel multiplier (1 OLED pixel → N×N square) |

---

## Writing a render script

A render script must expose a single function:

```python
def render(display):
    ...
```

`display` is an `OLED128x64Bicolor` instance.  The constants
`SSD1306_WHITE`, `SSD1306_BLACK`, and `SSD1306_INVERSE` are available
in `oled_svg`.

### Minimal example

```python
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from oled_svg import SSD1306_WHITE, SSD1306_INVERSE

def render(display):
    display.clearDisplay()

    # Yellow zone – centered title
    title = "BAME V2"
    x = (128 - len(title) * 6) // 2
    display.setTextSize(1)
    display.setTextColor(SSD1306_WHITE)
    display.setCursor(x, 4)
    display.print(title)

    # Blue zone – data lines
    display.setCursor(0, 16)
    display.print("13.2V   -5.4A")

    display.setCursor(0, 26)
    display.print("Cap: 82Ah  72%")

    # Simple gauge bar
    display.drawRect(1, 40, 126, 10, SSD1306_WHITE)
    display.fillRect(2, 41, 90, 8, SSD1306_WHITE)

    display.display()
```

### Available methods

| Method | Description |
|--------|-------------|
| `clearDisplay()` | Clear all pixels |
| `setCursor(x, y)` | Set text cursor position |
| `setTextSize(n)` | Set font scale (1 = 6×8 px per char) |
| `setTextColor(color, bg=None)` | Set text foreground / optional background color |
| `print(text)` | Print text at current cursor (advances cursor) |
| `drawPixel(x, y, color)` | Draw a single pixel |
| `drawRect(x, y, w, h, color)` | Rectangle outline |
| `fillRect(x, y, w, h, color)` | Filled rectangle |
| `drawLine(x0, y0, x1, y1, color)` | Bresenham line |
| `display()` | No-op (exists for API compatibility) |
| `to_svg(scale, background)` | Return SVG string |
| `write_svg(path, scale, background)` | Write SVG to file |

### Color constants

| Constant | Value | Meaning |
|----------|-------|---------|
| `SSD1306_WHITE` | 1 | Pixel ON (zone color) |
| `SSD1306_BLACK` | 0 | Pixel OFF (black) |
| `SSD1306_INVERSE` | 2 | XOR current pixel state |

---

## Examples

```bash
# SVG at 4× scale
python sim/render_screen.py sim/screens/info_cal.py --format svg --out out.svg --scale 4

# PNG at 6× scale (requires cairosvg)
python sim/render_screen.py sim/screens/info_cal.py --format png --out out.png --scale 6

# Run the example script standalone
python sim/screens/info_cal.py --format svg --out info_cal.svg
```

The resulting SVG can be embedded directly in Markdown:

```markdown
![OLED snapshot](out.svg)
```
