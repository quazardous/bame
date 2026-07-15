# Changelog

## v2.6

### INA226 diagnostic helper

New standalone firmware (`src/diag_ina226.cpp`, built via the `diag-prod` / `diag-nano` envs — `build_src_filter` compiles only it) for bench-debugging an INA226 install when the main firmware sits on "No Battery". It checks the sensor ACKs on I2C at 0x40 and matches its manufacturer/die IDs, shows a basic **live bus voltage + current** readout on the OLED, and — when the chip answers but reads ~0 V — points at the VBUS wiring. If 0x40 doesn't answer it runs an I2C bus scan and lists the addresses found in big font, so "not powered / not wired" (only `3C`) is distinguishable from "wrong address" (e.g. `3C 44`). Mirrors everything to Serial at 115200.

The base `build_src_filter` keeps `diag_ina226.cpp` out of every normal build (it has its own `setup()`/`loop()`).

## v2.5 — plug-and-forget (`v2` branch)

### No runtime UI

The device is installed once and left to run, so the whole runtime UI is gone: the settings menu, the Foxeer Key23 keypad + its ADC calibration, and the action button. Capacity starts at the compile-time `BATTERY_CAPACITY_AH` and refines itself; there is no manual "Battery full" or "Reset ALL". The OLED still sleeps/wakes on electrical activity. Frees ~9 % of flash.

### EEPROM: wear-leveled ring buffer with CRC

The fixed-address, magic-byte saves (one cell rewritten every 5 min → ~1-year wear, and a reset mid-write could leave a torn value that still passed the magic check) are replaced by a **32-slot ring buffer**. One record — coulomb counter, learned capacity, and the two-anchor state — rotates across the slots, each stamped with a sequence number and a CRC8. On boot the newest CRC-valid slot wins; a torn write fails the CRC and the previous slot is used. Wear is spread ~32× (decades), the periodic write is skipped when nothing moved (an idle parked van doesn't burn the ring), and the two-anchor measurement now survives a reset. Ring-buffer logic validated by a host test (roundtrip, wraparound, torn-write fallback, blank-EEPROM defaults).

## v2.4

### Bidirectional, aging-aware capacity learning (BUS)

Raise-only learning could only ever *raise* the estimate, so an aging pack kept an optimistic capacity forever — bad for an unattended install. BUS installs now learn capacity from **two rest-OCV anchors**: FULL (top plateau, ≥ 3.40 V/cell) and a new **KNEE** anchor (bottom of the LFP curve, ≤ 3.05 V/cell at rest). Between two opposite anchors the exact Ah moved (BUS sees all current) divided by the SOC swing read from the OCV curve gives an absolute capacity, EWMA-smoothed — so the estimate moves **up and down** and tracks a pack that loses capacity over the years.

- Cycles that never rest at the knee (soft usage) produce no measurement → the estimate holds, never wrongly lowered.
- Partial "trip charges" that never reach full don't corrupt anything: BUS coulomb counting integrates them, and the measurement only cares about the Ah between two anchors, not the path. A periodic full charge (leaving/returning from a trip) supplies the top anchor and re-syncs SOC.
- LOAD installs keep the raise-only amplitude-max learning — the charger is invisible to the shunt there, so there is no bottom-anchor charge leg to average against.
- New `bame_config_t` tunables: `v_knee_per_cell` (3.05), `cap_ewma_alpha` (0.35).

Validated in `sim/aging_proto.py` (drives the real C core): a pack aging 2.0 → 1.4 Ah is tracked to ~2 % vs ~24 % for raise-only, stable with no aging, and no false drift on knee-less usage.

Anchor state is RAM-only for now; the learned capacity itself persists in EEPROM. Persisting the in-flight anchor pair rides with the planned EEPROM hardening (see ROADMAP).

## v2.3

### No-battery detection is now voltage-gated

`battery_present` used to latch true on the first tick regardless of voltage — correct in prod (BaMe is powered by the battery it measures, so if it runs a battery is present) but wrong on the bench, where the MCU is powered separately and nothing is on the shunt: the "No Battery" splash never showed. Presence now tracks the bus voltage (≥ 1 V). In prod it stays true for the whole run exactly as before; a reconnect keeps the running coulomb count instead of snapping back to full.

### Screen auto-off (activity-driven, no wake button)

The OLED now sleeps (`SSD1306_DISPLAYOFF`, ~10 µA — the panel is self-emissive, there is no backlight to switch) after 2 minutes with no electrical activity. Since the install has no wake button, the wake source is the measurement itself: any current past the dead-band (charge **or** discharge) or any voltage step > 50 mV wakes the panel immediately and re-arms the 2-minute timer. Power-on arms it too, so the screen stays on for 2 minutes after boot. Full off, no dim; the sleeping panel skips its I²C repaint, and a future action button also wakes it.

## v2.2

### Firmware version on the no-battery splash

When no battery is detected the "No Battery" text blinks as before, but the firmware version now shows fixed (non-blinking) in a small font at the bottom-right corner, so a glance at an idle unit tells you what it's running.

### Single source of version

`BAME_VERSION` now lives only in `src/bame_state.h`. Removed the stale duplicate in `menu.cpp` (which still read `2.0-wip`, so the settings-menu title disagreed with the rest of the firmware) and the local copy in `main.cpp`. The splash, the serial banner and the menu title now read the same constant. `sim/render_screens.py` mirrors it, and the menu mockups use it instead of a hardcoded string.

### Reliable USBasp flashing on the ATmega328PB

The prod envs drove the USBasp ISP clock at `-B 4` (~187 kHz), which produced flash verification mismatches on the ATmega328PB. Lowered to `-B 8` (~93 kHz) — `make upload ENV=prod-*` now flashes and verifies cleanly.

## v2.1

### Amplitude-max capacity learning

The BMS-cutoff measurement path is dropped: BaMe powers off the very battery it measures, so a cutoff is a power loss the firmware reboots through instead of observing. Capacity learning now hangs off the **full** event — at each full, the cycle's peak depth of discharge is compared to the current estimate, and the estimate is raised to match if the cycle delivered more. Raise-only, so it converges upward over cycles and a shallow cycle never walks it back.

Docs (README, QUICKSTART, ROADMAP) realigned with this — all "full → BMS cutoff records the capacity" wording is gone.

### Sim

- `sim/optimize.py` rebuilt on the real C core: the genetic algorithm now evolves the actual `bame_config_t` thresholds (`v_full_per_cell`, `i_rest`, `full_rest_ms`, `v_rise_partial`, `v_disconnect_drop`, `ext_rearm_ms`) via ctypes, so a winning genome maps 1:1 onto `bame_config_defaults()`. Fitness includes mid-cycle SOC tracking error, not just end-of-run values. New `make sim-opt` target.
- `run_one_cycle()` in `sim/calibration_sim.py` accepts an optional `on_tick` callback for per-tick sampling.

## v2.0

Coulomb counting is the only source of SOC. Voltage is a display value plus a trigger for two events:

- **Charger disconnect at top voltage** → "battery full", SOC reset to 100%.
- **Voltage collapse to ~0** → BMS cutoff, the Ah delivered since the last "full" event is recorded as the capacity for that cycle (replaced by amplitude-max learning in v2.1).

Capacity refines as cycles accumulate. Always-on (no deep sleep), so the integration never has to be guessed across a wake-up.

### LOAD-mode external-charge handling

In LOAD installs the charger is wired off the shunt, so the current sensor never sees the charge. Firmware infers charger activity from voltage:

- **Rapid drop > 0.5 V** (voltage can't keep up with slow EWMA) → charger unplugged, exit charging state, coulomb integration resumes.
- **Rapid rise > 0.5 V at top voltage** → charger plugged in, force entry into charging state.
- **Sustained 15 s below top OCV** → re-arm (after a proper dip, next plug can trigger). Filters out the brief LFP rebond that follows a disconnect (voltage dips to 13.4 V for a few seconds then recovers to 13.7-13.9 V).

While in the charging state, coulomb integration is frozen so sensor noise / offset doesn't pull the counter below the obviously-full state.

### Architecture

Algorithm lives in `src/bame_core.c` — pure C, no Arduino deps. Linked into the firmware AND into a host shared library (`sim/bame_core.dll`) that Python loads via ctypes. Same code runs on the AVR and in `sim/calibration_sim.py`.

### Build

Per-install envs in `platformio.ini`: `nano-bus-4s`, `nano-load-4s`, `prod-bus-8s`, etc. Picked at flash time via `BAME_CELLS` and `BAME_WIRING_BUS`. `Makefile` + `Makefile.local` pattern for per-user defaults. `setup.ps1` one-shot installs Python / Pillow / MSYS2 gcc on Windows.
