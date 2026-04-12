# BAME - Battery Monitor

A small battery monitor for the cheap, unbranded LiFePO4 packs you find in camping vans, solar setups and off-grid boxes. Those batteries don't talk — no BMS output, no datasheet, often a capacity that's generously rounded up. BAME watches the current and voltage and figures out, over time, how much energy actually fits inside.

Plug it in on your 12V/24V line with an INA226 shunt and an OLED, tell it roughly how many cells are in series and the nominal Ah written on the sticker, then forget about it. As you use the battery normally, each discharge + rest cycle refines the real capacity estimate and the State of Charge stays honest.

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

Between two moments where the battery is truly at rest, BAME measures how many coulombs flowed and how far the SOC dropped — the ratio gives the real capacity. Segments start short (2 minutes) and double each cycle, so the estimate sharpens quickly then keeps refining. Every completed segment is weighted by its delta SOC, so long discharges count more than short ones.

A segment is dropped if the battery gets recharged mid-way (>1A sustained). Brief current spikes from a fridge compressor or pump are tolerated.

### Detecting "real rest"

LFP cells need a quiet window to show their true voltage. BAME keeps a rolling 160-second history and only trusts a reading when the current is low **and** the voltage has stayed flat within 20 mV over that window. That rules out the false calms between compressor cycles, which is where naive monitors get the capacity wrong.

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
