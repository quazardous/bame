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

## 3. First-time setup

On Windows:

```powershell
.\setup.ps1
```

Installs Python, Pillow (for screenshot rendering) and MSYS2 (which
provides gcc for building the shared C core used by the simulator).

On Linux/macOS the only deps are Python, Pillow, gcc — all available via
your package manager.

## 4. Build & flash

[PlatformIO](https://platformio.org/) is the build system, wrapped by a
small Makefile for everyday tasks.

```bash
make              # build the default env
make upload       # build + flash
make monitor      # serial monitor
make list-envs    # what env names are available
```

The default env is `nano-bus-4s`. To use a different one (your install),
copy the template:

```bash
cp Makefile.local.example Makefile.local
# edit Makefile.local, set ENV = nano-load-4s (or whatever matches)
```

`Makefile.local` is gitignored — your setup stays yours.

Or override on the fly:

```bash
make ENV=nano-load-4s upload
```

Each env outputs `.pio/build/<env-name>/firmware.hex`. The env name encodes
the config so you can keep multiple firmwares around without confusion.

Available variants (run `make list-envs`):

- `nano-bus-4s`   — default, Arduino Nano dev, 12 V BUS install
- `nano-load-4s`  — Arduino Nano dev, 12 V LOAD install
- `nano-bus-8s`   — Arduino Nano dev, 24 V BUS install
- `prod-bus-4s`   — production board (USBasp programmer), 12 V BUS
- `prod-load-4s`  — production board, 12 V LOAD
- `prod-bus-8s`   — production board, 24 V BUS

## 5. First boot + settings menu

Long-press CENTER to open the settings menu. Items:

- **Capacity (Ah)** — sticker value of your battery. Used as the starting
  reference until a discharge cycle to BMS cutoff measures the real
  capacity and replaces it (the `*` next to the capacity display goes away
  at that point).
- **Battery full** — manual "SOC = 100%" declaration. Use after a charge
  that didn't trigger the auto-detect (= the charging icon didn't appear),
  or to clear a `?` next to the Ah reading.
- **Reset ALL** — wipes EEPROM (capacity, coulomb count, keypad
  calibration) and reboots.

Compile-time (set at build via `platformio.ini`, not in the menu):

- **Cell count** → `BAME_CELLS`
- **Wiring** → `BAME_WIRING_BUS` (1 for BUS, 0 for LOAD)

## 6. Add your own env (custom cell count etc.)

Open `platformio.ini`, copy any `[env:*]` block, change the env name and
the `-DBAME_CELLS=<n>` / `-DBAME_WIRING_BUS=<0|1>` flags. That's it.

```ini
[env:nano-bus-7s]
extends = hw_nano
build_flags = ${hw_nano.board_flags} -DBAME_WIRING_BUS=1 -DBAME_CELLS=7
```

## 7. Simulator (optional)

`sim/calibration_sim.py` drives the real C core (loaded as a shared
library via ctypes) against a synthetic LFP battery, so you can check how
the firmware will behave on a full → cutoff → recharge cycle without
waiting hours for your real pack to go through it.

```bash
make core-lib     # compile sim/bame_core.dll once
make sim-cal      # run a multi-cycle convergence test
```

The sim and firmware share the same `src/bame_core.c` — there's no Python
re-implementation to keep in sync.

## 8. What if something goes wrong

- `?` next to the Ah reading → SOC drifted from reality (e.g. invisible
  charge in LOAD mode). Do a complete charge until the charging icon
  appears and sticks, or declare "Battery full" manually. Both clear the
  `?`.
- `*` next to the capacity at rest → no cycle measured yet. Run one full
  charge → full discharge cycle and it'll disappear.
- Icon doesn't show when the charger is on (LOAD mode) → voltage needs to
  be ≥ top OCV (3.40 V/cell, = 13.6 V on 4S) with rest current, sustained.
  Charger at CV phase pushing 14.4 V should trigger it within ~30 s.
- Source of truth for the firmware behavior is `src/bame_core.c` +
  `src/main.cpp`. The core algorithm is only ~200 lines of pure C.
