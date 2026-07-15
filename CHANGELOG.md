# Changelog

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
