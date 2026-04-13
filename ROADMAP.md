# Roadmap

Ideas not yet implemented. No timeline.

## v2 — pure coulomb counting

Drop voltage-based SOC estimation and the segment-based capacity calibration. The current sensor is the only source of truth for SOC. Voltage stays as a display value and is used only for two events: detecting "battery full" (charger disconnect at top voltage) and "battery empty" (BMS cutoff = voltage drops to ~0). The Ah counted between those two events IS the measured capacity for that cycle. Average across cycles to refine. Always-on (no deep sleep — can't integrate while asleep).

The v1 line (voltage SOC blend + segment calibration) lives on in branch `v1` for users who can't tolerate manual cycles.
