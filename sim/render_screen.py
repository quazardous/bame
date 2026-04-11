#!/usr/bin/env python3
"""
render_screen.py – CLI runner for OLED screen render scripts.

Usage
-----
    python sim/render_screen.py <script.py> [--format svg|png] [--out <path>] [--scale N]

The script must expose a function:

    def render(display):
        ...

where ``display`` is an :class:`oled_svg.OLED128x64Bicolor` instance
pre-loaded with the Adafruit-compatible constants
(``SSD1306_WHITE``, ``SSD1306_BLACK``, ``SSD1306_INVERSE``).

Examples
--------
    # Produce an SVG file
    python sim/render_screen.py sim/screens/info_cal.py --format svg --out out.svg

    # Produce a PNG (requires cairosvg: pip install cairosvg)
    python sim/render_screen.py sim/screens/info_cal.py --format png --out out.png --scale 4
"""

import argparse
import importlib.util
import os
import sys
import tempfile


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _load_script(path: str):
    """Dynamically load a Python script and return its module object."""
    path = os.path.abspath(path)
    if not os.path.isfile(path):
        print(f"Error: script not found: {path}", file=sys.stderr)
        sys.exit(1)
    spec = importlib.util.spec_from_file_location("_render_script", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _svg_to_png(svg_str: str, out_path: str) -> None:
    """Convert SVG string to PNG using cairosvg."""
    try:
        import cairosvg
    except ImportError:
        print(
            "Error: cairosvg is required for PNG output.\n"
            "Install it with:  pip install cairosvg",
            file=sys.stderr,
        )
        sys.exit(1)

    cairosvg.svg2png(bytestring=svg_str.encode("utf-8"), write_to=out_path)
    print(f"PNG written to {out_path}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Render a BAME OLED screen script to SVG or PNG."
    )
    parser.add_argument(
        "script",
        help="Path to a Python render script exposing render(display).",
    )
    parser.add_argument(
        "--format",
        choices=["svg", "png"],
        default="svg",
        help="Output format (default: svg).",
    )
    parser.add_argument(
        "--out",
        default=None,
        help="Output file path (default: <script_basename>.<format>).",
    )
    parser.add_argument(
        "--scale",
        type=int,
        default=4,
        help="Pixel scale factor (default: 4).",
    )
    args = parser.parse_args()

    # Ensure sim/ is on sys.path so scripts can import oled_svg
    sim_dir = os.path.dirname(os.path.abspath(__file__))
    if sim_dir not in sys.path:
        sys.path.insert(0, sim_dir)

    # Determine default output path
    out_path = args.out
    if out_path is None:
        base = os.path.splitext(os.path.basename(args.script))[0]
        out_path = f"{base}.{args.format}"

    # Load and validate the render script
    mod = _load_script(args.script)
    if not hasattr(mod, "render") or not callable(mod.render):
        print(
            "Error: the script must define a callable 'render(display)'.",
            file=sys.stderr,
        )
        sys.exit(1)

    # Build the display and call render
    from oled_svg import OLED128x64Bicolor
    disp = OLED128x64Bicolor()
    disp.clearDisplay()

    mod.render(disp)

    # Export
    if args.format == "svg":
        disp.write_svg(out_path, scale=args.scale)
    else:  # png
        svg_str = disp.to_svg(scale=args.scale)
        _svg_to_png(svg_str, out_path)


if __name__ == "__main__":
    main()
