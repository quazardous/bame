# Roadmap

Ideas not yet implemented. No timeline.

## Long-term drift check

After weeks of real-world use, see whether the coulomb integrator drifts meaningfully between "battery full" auto-detects. If it does, tighten the `i_rest` dead band or add a slow auto-zero on the current offset during sustained idle.

## Non-LFP chemistries

The top-OCV event and BMS cutoff logic is LFP-specific (flat curve with a distinct top plateau at 3.40 V/cell). Adapting to AGM or lead-acid would mean re-thinking the "battery full" trigger.

## Multi-battery

Currently one shunt, one pack. Future: wire two INA226 boards and track two banks independently (for example, starter + house batteries in a van).
