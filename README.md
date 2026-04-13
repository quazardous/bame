# BaMe - Battery Meter

> **Want to build & flash your own?** → [QUICKSTART.md](QUICKSTART.md)

A small, unambitious side project to put repurposed "dumb" golf-cart LiFePO4 batteries to work in a camping van. No BMS, no data interface, no datasheet — just a cheap pack of cells and a sticker with a capacity that's probably optimistic.

Yes, buying a proper modern battery with its own BMS and bluetooth app would do this better. But it's more fun to poke at the problem with an Arduino, an INA226 shunt and a small OLED than to click "add to cart". If you like that kind of thing, this is that kind of thing.

BaMe watches current through the shunt, integrates it, and shows you a gauge, remaining Ah, voltage, watts and estimated time. On a discharge cycle that ends at the BMS cutoff, it measures the true capacity of your pack — the value on the sticker is usually wrong.

## Features

- **Pure coulomb counting** — charge in, charge out, tracked continuously. No voltage-SOC trick on the flat LFP curve.
- **Capacity learned from real cycles** — each "battery full → BMS cutoff" sequence records the Ah delivered. Averaged over cycles, the learned value converges to the real capacity.
- **Auto-detect "battery full"** — voltage at top OCV with low current, sustained, resets the SOC to 100%. You can also declare it manually from the menu.
- **Auto-detect "charger attached"** (LOAD install) — voltage kicks >0.5 V on plug in, drops >0.5 V on unplug. Hysteresis filters the LFP rebond.
- **Smoothed watts & autonomy** — EWMA on the current so a cycling fridge doesn't make the display jump.
- **Configurable at build time** — cell count, wiring topology, voltage window. See `platformio.ini`.

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

### Wiring

Two topologies, picked at compile time.

- **BUS** — shunt is on the battery bus, every current (load and charge) flows through it. Coulomb counting is bidirectional. The natural choice when you build from scratch.
- **LOAD** — shunt is on the load side, charger bypasses it. BaMe never sees charge current. Used when retrofitting into an existing install.

### Capacity measurement

When the battery goes from full to BMS cutoff, the Ah delivered through the shunt (BUS) or off a full reference (LOAD) is the measured capacity for that cycle. Multiple cycles blend into a running estimate. Until the first cycle completes, BaMe shows the sticker value with a `*`.

### SOC uncertainty

Voltage never "corrects" the coulomb counter — LFP's flat curve makes that correction actively harmful. If something happens BaMe can't quantify (LOAD install with an invisible partial charge, or missing events after a reset), a `?` appears next to the Ah reading to flag that the displayed SOC drifted from reality. The next confirmed "battery full" event clears it.

### Events BaMe listens for

- **Full** — voltage ≥ top OCV with rest current, sustained → SOC = 100%, start of a new cycle
- **BMS cutoff** — voltage collapses → close the cycle, record the capacity sample
- **Charger plug / unplug** (LOAD only) — rapid voltage rise / drop, detected via a slow-moving average that can't keep up with real chargers

## Build

[PlatformIO](https://platformio.org/) wrapped in a small Makefile. See [QUICKSTART.md](QUICKSTART.md) for the quick path.

```bash
make                     # build the default env (nano-bus-4s)
make upload              # upload over USB
make ENV=nano-load-4s upload
make list-envs           # show all available variants
```

Each env name encodes hardware × wiring × cell count (e.g. `nano-load-4s` = Arduino Nano, LOAD install, 4-cell LFP / 12 V).

## Controls

| Action | Effect |
|--------|--------|
| Hold CENTER 0.5 s | Open settings menu |
| In menu: UP / DOWN | Navigate / increment / decrement |
| In menu: CENTER | Enter edit / confirm action |
| In menu: LEFT | Cancel edit / exit menu |
| Hold any button at boot 5 s | Keypad re-calibration |

## Settings menu

- **Capacity** — sticker value of your battery (Ah). Used as the starting reference until a cycle measures the real capacity.
- **Battery full** — manual declaration that the battery is at 100%. Use after a charge that didn't trigger the auto-detect.
- **Reset ALL** — wipes EEPROM (capacity, coulomb count, keypad) and reboots.
- Last row — voltage + SOC% read-only, useful as a quick info row.

## License

MIT — see [LICENSE.txt](LICENSE.txt)
