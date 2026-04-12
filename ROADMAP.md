# Roadmap

Ideas not yet implemented. No timeline.

## Full-cycle coulomb counter

Direct brute measurement of Ah delivered between charger removal and BMS cutoff, as a ground-truth check against the calibrated capacity. Incompatible with deep sleep; flash budget tight on prod.

## Split Vmin tracking

Two observed Vmin values: one updated from any reading (under load, informational), one updated only at confirmed rest (reliable reference for the full-cycle counter).
