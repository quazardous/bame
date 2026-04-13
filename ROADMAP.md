# Roadmap

Ideas not yet implemented. No timeline.

## v2 — pure coulomb counting

The current sensor is the only source of truth for SOC. Voltage is a display value, plus a trigger for two events: charger disconnect at top = "battery full" (SOC reset to 100%), and voltage collapse to ~0 = "BMS cutoff" (Ah delivered since the last full event = measured capacity for that cycle). Average across cycles to refine. Always-on, no deep sleep.
