# Changelog

## v2.0

Coulomb counting is the only source of SOC. Voltage is a display value plus a trigger for two events:

- **Charger disconnect at top voltage** → "battery full", SOC reset to 100%.
- **Voltage collapse to ~0** → BMS cutoff, the Ah delivered since the last "full" event is recorded as the capacity for that cycle.

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
