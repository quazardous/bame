# Roadmap

Ideas not yet implemented. No timeline.

## Bidirectional learning for LOAD installs

BUS learns capacity bidirectionally via a FULL + KNEE anchor pair. In LOAD the charge leg is invisible, but the *discharge* leg (FULL → KNEE) is still measurable — the same two-anchor method could apply on the discharge side alone.

## Lower the current dead band

The ±50 mA dead band rejects sensor noise, but it also means anything under 50 mA is never counted — including BaMe's own draw (MCU + INA226 + buck, ~10 mA on the 12 V side, 24/7 ≈ 0.25 Ah/day). The INA226's resolution is far better than 50 mA, so the band could likely drop to ~10-20 mA now that the offset auto-zero no longer eats real loads (v2.13). Worth measuring the actual sensor noise floor first.

## Non-LFP chemistries

The top-OCV "battery full" event is LFP-specific (flat curve with a distinct top plateau at 3.40 V/cell). Adapting to AGM or lead-acid would mean re-thinking the "battery full" trigger.

## Multi-battery

Currently one shunt, one pack. Future: wire two INA226 boards and track two banks independently (for example, starter + house batteries in a van).
