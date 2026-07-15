# BaMe - Battery Meter

> **Want to build & flash your own?** → [QUICKSTART.md](QUICKSTART.md)

A small, unambitious side project to put repurposed "dumb" golf-cart LiFePO4 batteries to work in a camping van. No BMS, no data interface, no datasheet — just a cheap pack of cells and a sticker with a capacity that's probably optimistic.

Yes, buying a proper modern battery with its own BMS and bluetooth app would do this better. But it's more fun to poke at the problem with an Arduino, an INA226 shunt and a small OLED than to click "add to cart". If you like that kind of thing, this is that kind of thing.

BaMe watches current through the shunt, integrates it, and shows you a gauge, remaining Ah, voltage, watts and estimated time. Real cycles teach it the true capacity of your pack — the sticker value is usually wrong — and on a BUS install the estimate even tracks *down* as the pack ages.

## Features

- **Pure coulomb counting** — charge in, charge out, tracked continuously. No voltage-SOC trick on the flat LFP curve.
- **Capacity learned from real cycles** — on a BUS install BaMe measures capacity between two rest-voltage anchors (the *full* plateau at the top of the LFP curve and the *knee* at the bottom) and averages it, so the estimate moves **up and down** and follows a pack that loses capacity as it ages. On a LOAD install (charger invisible to the shunt) it's raise-only: the estimate rises to the deepest discharge seen and never walks back.
- **Plug-and-forget** — no buttons, no menu. Cell count, wiring and starting capacity are set at build time; wire it in and leave it. The OLED sleeps after 2 min and wakes on any electrical activity.
- **Auto-detect "battery full"** — voltage at top OCV with low current, sustained, resets the SOC to 100%.
- **Auto-detect "charger attached"** (LOAD install) — voltage kicks >0.5 V on plug in, drops >0.5 V on unplug. Hysteresis filters the LFP rebond.
- **Smoothed watts & autonomy** — EWMA on the current so a cycling fridge doesn't make the display jump.
- **Configurable at build time** — cell count, wiring topology, starting capacity. See `platformio.ini`.

## Screenshots

### Main display — discharging

![Discharging](docs/screenshots/main_discharge.png)

### Main display — charging

![Charging](docs/screenshots/main_charge.png)

### Main display — at rest

![At rest](docs/screenshots/main_rest.png)

### No battery

![No battery](docs/screenshots/no_battery.png)

## How it works

### Wiring

Two topologies, picked at compile time.

- **BUS** — shunt is on the battery bus, every current (load and charge) flows through it. Coulomb counting is bidirectional. The natural choice when you build from scratch.
- **LOAD** — shunt is on the load side, charger bypasses it. BaMe never sees charge current. Used when retrofitting into an existing install.

### Capacity measurement

BaMe powers itself from the very battery it measures, so a BMS cutoff is a power-loss event it can never witness. Capacity is instead learned from rest-voltage anchors on the steep parts of the LFP curve:

- **BUS** — every current passes the shunt, so the Ah moved between the *full* anchor (top OCV plateau) and the *knee* anchor (bottom of the curve, read at rest) is measured exactly. That Ah over the SOC swing gives an absolute capacity, averaged (EWMA) across cycles — it moves **up and down**, so it tracks a pack that loses capacity as it ages. Cycles that never rest at the knee produce no measurement, so a light-use estimate is never wrongly lowered.
- **LOAD** — the charger bypasses the shunt, so there is no bottom-anchor charge leg to measure. BaMe falls back to raise-only: at each full event it raises the estimate to the deepest discharge seen since the last full, never lowering it.

Until a cycle has measured the capacity, BaMe shows the sticker value with a `*`.

### SOC uncertainty

Voltage never "corrects" the coulomb counter — LFP's flat curve makes that correction actively harmful. If something happens BaMe can't quantify (LOAD install with an invisible partial charge, or missing events after a reset), a `?` appears next to the Ah reading to flag that the displayed SOC drifted from reality. The next confirmed "battery full" event clears it.

### Events BaMe listens for

- **Full** — voltage ≥ top OCV with rest current, sustained → SOC = 100%, and the top capacity anchor
- **Knee** (BUS) — rest voltage at the bottom of the LFP curve → the low anchor that lets capacity be measured against the full anchor
- **Charger plug / unplug** (LOAD only) — rapid voltage rise / drop, detected via a slow-moving average that can't keep up with real chargers

A BMS cutoff is deliberately *not* an event: it cuts BaMe's own power supply, so the firmware reboots instead of observing it — which is why capacity is learned from the rest-voltage anchors above, not from an "empty" sample.

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

None. BaMe is plug-and-forget: no buttons, no menu. It's installed once and left
to run. Capacity starts at the compile-time value (`BATTERY_CAPACITY_AH` /
`BAME_CELLS` in `platformio.ini`) and refines itself from real cycles — nothing
to configure at runtime. The OLED sleeps after 2 min of no electrical activity
and wakes on any current or voltage change (see the display behaviour above).

## License

MIT — see [LICENSE.txt](LICENSE.txt)
