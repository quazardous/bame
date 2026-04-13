# BaMe - Battery Meter

A small, unambitious side project to put repurposed "dumb" golf-cart LiFePO4 batteries to work in a camping van. No BMS, no data interface, no datasheet — just a cheap pack of cells and a sticker with a capacity that's probably optimistic.

Yes, buying a proper modern battery with its own BMS and bluetooth app would do this better. But it's more fun to poke at the problem with an Arduino, an INA226 shunt and a small OLED than to click "add to cart". If you like that kind of thing, this is that kind of thing.

BaMe watches voltage and current, figures out over time how much energy actually fits inside, and gives you a gauge, a State of Charge, and an estimated time remaining that doesn't jump around every time the fridge kicks in.

## Features

- **Auto capacity calibration** — learns the true Ah of the pack from real usage, no lab cycle needed
- **Honest SOC** — coulomb counting + voltage lookup + rest-based correction, works even on the flat LFP curve
- **Tolerates real life** — compressor cycling, brief loads and partial recharges don't poison the estimate
- **OLED display** with SOC gauge, voltage, current, power, time remaining
- **Eco mode** — deep sleep with adaptive wake interval for permanent install
- **Configurable** cell count (1-16S), nominal capacity, usable voltage window

## Screenshots

### Main display — discharging

![Discharging](docs/screenshots/main_discharge.png)

### Main display — charging

![Charging](docs/screenshots/main_charge.png)

### Main display — at rest

![At rest](docs/screenshots/main_rest.png)

### No battery

![No battery](docs/screenshots/no_battery.png)

### Settings menu

![Menu](docs/screenshots/menu.png)

### Editing a value

![Editing](docs/screenshots/menu_edit.png)

## How it works

### Calibration

Between two moments where the battery is truly at rest, BaMe measures how many coulombs flowed and how far the SOC dropped — the ratio gives the real capacity. Segments start short (2 minutes) and double each cycle, so the estimate sharpens quickly then keeps refining. Every completed segment is weighted by its delta SOC, so long discharges count more than short ones.

A segment is dropped if the battery gets recharged mid-way (>1A sustained). Brief current spikes from a fridge compressor or pump are tolerated.

### Detecting "real rest"

LFP cells need a quiet window to show their true voltage. BaMe only trusts a reading when the voltage has settled and the current has stayed low — not just at one instant, but continuously over the recent past. That rules out the false calms between compressor cycles, which is where naive monitors get the capacity wrong.

### Steady watts and autonomy

A fridge compressor pulsing on and off would normally make the watts and "time remaining" readings jump all over the place. BaMe smooths them out so you see the real average draw, not the instantaneous spikes.

### SOC estimation

When rest is confirmed, the SOC nudges toward the voltage-based estimate (8% blend). That corrects the slow drift of coulomb counting without jumping around on the flat middle of the LFP curve.

### Voltage calibration

The top-of-charge reference converges toward the actual observed rest voltage, so the SOC curve adapts to your specific pack without you touching anything.

## Build

Requires [PlatformIO](https://platformio.org/).

```bash
# Prototype (Arduino Nano)
pio run -e nano -t upload

# Production (ATmega328PB + USBasp)
pio run -e prod -t upload
```

## Controls

| Action | Effect |
|--------|--------|
| Hold center 0.5s | Open settings menu |
| Hold center 3s | Enter deep sleep |
| Hold any button at boot 5s | Keypad calibration |
| UP/DOWN in menu | Navigate / change value |
| CENTER in menu | Enter edit / save |
| LEFT in menu | Cancel edit / go back |

## License

MIT — see [LICENSE.txt](LICENSE.txt)
