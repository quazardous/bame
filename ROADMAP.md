# Roadmap

Ideas not yet implemented. No timeline.

## EEPROM robustness (reset-hardening)

The coulomb counter is saved every 5 min at a fixed address with `EEPROM.write` for the magic byte — so one cell is rewritten ~105k times/year vs ~100k endurance, and a reset mid-write can leave a torn (half-updated) float that passes the magic check. Plan: a wear-leveled ring buffer of slots, each with a sequence number + CRC; pick the highest valid slot on boot, `EEPROM.update` throughout. Fold in persistence of the two-anchor state (`last_anchor_kind`, `coulomb_at_last_anchor`, `soc_at_last_anchor`) so an in-flight capacity measurement survives a reset.

## Bidirectional learning for LOAD installs

v2.4 made capacity learning bidirectional on BUS via a FULL + KNEE anchor pair. In LOAD the charge leg is invisible, but the *discharge* leg (FULL → KNEE) is still measurable — the same two-anchor method could apply on the discharge side alone.

## Long-term drift check

After weeks of real-world use, see whether the coulomb integrator drifts meaningfully between "battery full" auto-detects. If it does, tighten the `i_rest` dead band or add a slow auto-zero on the current offset during sustained idle.

## Non-LFP chemistries

The top-OCV "battery full" event is LFP-specific (flat curve with a distinct top plateau at 3.40 V/cell). Adapting to AGM or lead-acid would mean re-thinking the "battery full" trigger.

## Multi-battery

Currently one shunt, one pack. Future: wire two INA226 boards and track two banks independently (for example, starter + house batteries in a van).
