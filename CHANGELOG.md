# Changelog

## v2.0-wip

Coulomb counting is the only source of SOC. Voltage is a display value plus a trigger for two events:

- **Charger disconnect at top voltage** → "battery full", SOC reset to 100%.
- **Voltage collapse to ~0** → BMS cutoff, the Ah delivered since the last "full" event is recorded as the capacity for that cycle.

Capacity refines as cycles accumulate. Always-on (no deep sleep), so the integration never has to be guessed across a wake-up.
