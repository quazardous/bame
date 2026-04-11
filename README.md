# BAME - Battery Monitor

Firmware for a LiFePO4 4S battery monitor with automatic capacity calibration.

## Features

- **SOC estimation** from voltage lookup + coulomb counting + rest blending
- **Auto capacity calibration** using exponential doubling: first estimate in ~1 min, accuracy improves with each discharge cycle
- **INA226 current auto-zero** eliminates offset drift for precise coulomb counting
- **Eco mode** (deep sleep) activates automatically once calibration is reliable
- **OLED display** with SOC gauge, voltage, current, power, time remaining
- **Menu system** for settings (capacity, eco mode, factory reset)
- **Adaptive deep sleep** with wake interval based on voltage rate of change

## How it works

The calibration system measures energy consumed (coulombs) between two rest voltage readings. It starts with a short segment (~1 min) and doubles the target each time, producing increasingly accurate capacity estimates. A partial recharge invalidates the current segment. Rest voltage readings require 5 seconds of stable current (<0.3A) to ensure accuracy on the flat LFP voltage curve.

## Build

Requires [PlatformIO](https://platformio.org/).

```bash
# Prototype (Arduino Nano)
pio run -e nano -t upload

# Production (ATmega328PB + USBasp)
pio run -e prod -t upload
```

## Menu access

- **Short press center (0.5s)**: open settings menu
- **Long press center (3s)**: enter deep sleep
- **Hold button at boot (5s)**: keypad calibration

## License

MIT
