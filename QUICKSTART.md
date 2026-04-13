# BaMe — Quickstart

You want to flash your own BaMe and not read 1000 lines of source. Here.

## 1. Pick your install

Two physical ways to wire BaMe into your battery setup. The firmware needs
to know which one you used.

```
BUS install                              LOAD install
(BAME_WIRING_BUS=1)                      (BAME_WIRING_BUS=0)

  Battery+                                Battery+
     │                                     │     │
     │                                     │     ├──► Charger
  [SHUNT]                                  │     │
     │                                  [SHUNT]  │
     ├──► Loads                            │     │
     └──► Charger                          ├──► Loads
                                           └──► (back to battery)
```

- **BUS** — every load AND charge current goes through the shunt. BaMe sees
  charge as negative current → unambiguous detection.
- **LOAD** — the charger is wired directly to the battery, bypassing the
  shunt. BaMe never sees charge current → has to detect charge from the
  voltage rising. Slightly slower, less reliable on noisy chargers.

Pick BUS if you have the choice. LOAD is for retrofits where you can't
re-route the charger.

## 2. Pick your cell count

Standard LFP packs:
- **4S** = 12 V system (most camping vans, golf-cart packs)
- **8S** = 24 V system (heavy-duty installs)
- Anything else: add an env in `platformio.ini`.

## 3. Build & flash

[PlatformIO](https://platformio.org/) is the build system.

```bash
# default — nano hardware, BUS, 4S
pio run -t upload

# explicit (recommended once you know your install)
pio run -e nano-bus-4s   -t upload    # default
pio run -e nano-load-4s  -t upload    # 12 V LOAD install
pio run -e nano-bus-8s   -t upload    # 24 V BUS install
pio run -e prod-bus-4s   -t upload    # production board (USBasp programmer)
```

Each env outputs `.pio/build/<env-name>/firmware.hex`. The env name encodes
the config so you can keep multiple firmwares around without confusion.

## 4. Configure on first boot

A few things stay runtime-configurable via the menu (long-press CENTER):

- **Capacity (Ah)** — the sticker value of your battery. The firmware will
  refine this over time via calibration; the user setting is just the
  starting point and the value reset-to.
- **V min / V max utile** (full build only) — your "safe" voltage window.
  Defaults to LFP nominal (3.00 V/cell low, 3.40 V/cell rest-OCV high).
- **Eco mode** (full build only) — toggles deep sleep on inactivity.

What was a menu item but is now compile-time:
- **Cell count** → set via `BAME_CELLS` at build, no menu item.
- **Wiring** → set via `BAME_WIRING_BUS`, no menu item.

## 5. Add your own env (custom cell count etc.)

Open `platformio.ini`, copy any `[env:*]` block, change the env name and
the `-DBAME_CELLS=<n>` / `-DBAME_WIRING_BUS=<0|1>` flags. That's it.

```ini
[env:nano-bus-7s]
extends = hw_nano
build_flags = ${hw_nano.board_flags} -DBAME_WIRING_BUS=1 -DBAME_CELLS=7
```

You can also override the user voltage window at build time if your pack
runs outside the LFP defaults:

```
build_flags = ... -DBAME_VMIN_UTILE=12.5 -DBAME_VMAX_UTILE=14.0
```

## 6. What if something goes wrong

- The flash usage is printed at the end of every `pio run` — if the
  ATmega328PB prod build hits 100% you've added too much code.
- Calibration takes multiple full discharge → rest cycles to converge.
  See `sim/calibration_sim.py` if you want to play with the algorithm
  offline.
- Source of truth for the firmware behavior is `src/main.cpp`. Each major
  block (display, calibration, trend detection) has a long comment at the
  top explaining what it does and why.
