# Changelog

## v1.15

- Fixed aberrant watts reading at rest (e.g. 500-1000W). Root cause: the SOC blend was silently rewriting `coulombCount` while rest was active, and the current-smoothing buffer was reading the same variable — every correction showed up as a fake current spike for the full 80s window, which also kicked the rest gate off and made calibration flicker.
- Introduced `coulombRaw`, a separate pure integrator never touched by the SOC blend. The current buffer (`chist[]`) now samples `coulombRaw` instead of `coulombCount`. `coulombCount` stays corrected for SOC/Ah display.
- `coulombRaw` renormalizes periodically (past ±100k A·s) by subtracting its value from every chist entry — deltas preserved, float precision stays tight.
- Replaced the regression slope for `cAvg` with a 2-point sliding window against the live `coulombRaw`, so watts and autonomy refresh every 100ms instead of stepping every 10s.
- Simulator updated: models the SOC blend, reproduces the 500W+ spike at rest entry, and has a `--use-raw` flag to validate the fix. Also shows a watts column and the STABLE/EXIT events caused by the polluted buffer.
- README: "How it works" section stays conceptual (no implementation jargon).
- Screenshots regenerated to target the PROD build (no dev-only cal counter / countdown); the render script now references the corresponding `main.cpp` line numbers for each element.

## v1.14

- Parallel current buffer sampled at the same 10s cadence as voltage. Stores snapshots of `coulombCount` (continuously integrated at 100 ms), so no aliasing on cyclic loads like a fridge compressor.
- Smoothed current = linear regression slope on the coulomb buffer (same formula as voltage trend). Reuses the existing Sx/D constants — math is symmetric.
- Watts display and autonomy (HH:MM) now use the smoothed current when the buffer is full, so compressor cycling stops making the readings jitter.
- Rest gate for calibration: replaced the instantaneous `|current|` check with `maxSliceI < VBAT_REST_CURRENT` — the maximum average current over any 10s slice of the buffer must stay below threshold. Calibration now opens/closes only when voltage flat AND current low for the whole 80s window.
- Python simulator (sim/trend_sim.py): added current profile, cAvg, maxSliceI, combined stable-zone gate.

## v1.13

- Tuned trend detection for realistic glaciere cycles: buffer 16→8 samples (80s window), slope threshold 10→5 mV/step. Catches 100 mV voltage swings that previously went undetected.
- Flat chrono replaces the stability-spread gate: calibration opens only after the voltage slope has stayed flat for 60s (configurable via FLAT_COUNTDOWN_MS).
- Retro-range back-dating: when the slope just flipped to flat, scan the buffer backwards and count how many newest samples stay within 20 mV. Backdate flatSince by that much so the chrono finishes sooner when the voltage was already quiet.
- Optimistic segment open (dev): calStartVoltage is set as soon as flat is detected, not after the full chrono. Discarded if the slope reverts before any discharge.
- Preempt / commit / rollback (dev): when the coulomb target is reached and the chrono starts, freeze a candidate segment end (voltage, coulombs). Commit on chrono elapse, rollback on voltage movement — lets the window extend naturally when no stable rest is available, improving the ratio Ah / ΔSOC%.
- Display: always-visible cal counter (blinks while waiting for rest), 60→0 flat countdown where the arrow would be, cleaner rendering order (one setTextSize(1) pass for everything after the big numbers).
- Trend detection parameters promoted to documented #defines at the top of main.cpp, compile-time folded Sx / D for the regression.
- Prod build: chrono + retro + preempt + cal-counter + countdown all gated behind #if BAME_DEV. Prod falls back to a direct stableRest gate and single-shot segment save to fit in flash.
- Python simulator (sim/trend_sim.py): ports the firmware logic including retro-range back-dating, stable-zone enter/exit events, and a glaciere cycle profile for parameter tuning.

## v1.12

- Voltage trend: linear regression over the full 16-sample buffer (slope in V/10s) replaces the split-average approximation
- Rest detection: stability gate (bufMax - bufMin < 20mV over 160s) replaces the 5s restSince timer
- Removed redundant vMinObserved, restSince, updateVoltageCalibration() — unified into the single stableRest pathway
- vbatTop slow-convergence inlined into the rest block
- Info line + V min menu label now read from bufMin (buffer-derived)
- Sim cleanup: removed the SVG-based renderer (oled_svg, bamegfx, render_screen, screens/); kept the standalone PNG emulator and updated it to v1.12 layout
- Stale screenshots removed from docs/screenshots
- README rewritten as a plain-language intro for camping-van / off-grid use
- Added ROADMAP.md (full-cycle coulomb counter, split Vmin tracking)
- Removed old isp_programmer/ subtree

## v1.11

- Removed vbatMaxSeen: unreliable due to LFP surface charge memory effect
- Battery "full" icon now uses theoretical threshold (cellCount × 3.40V)
- Freed ~300 bytes flash and 4 bytes RAM
- EEPROM addr 37-41 freed

## v1.10

- Added vbatMaxSeen: captures OCV after charge plateau (persistent in EEPROM)
- Battery icon: static full/partial states based on voltage and charging detection
- Battery icon position fixed bottom-right, calibration counter bottom-left
- Voltage trend arrow: rounded delta for cleaner flat detection
- Refactored: setCapacity() and resetCalibration() helpers
- Removed unused lastRestVoltage variable
- constrain() for hoursLeft clamping
- 2-state charging battery icon (partial when charging, full when at max voltage)
- Known issue: vbatMaxSeen unreliable due to LFP surface charge memory effect

## v1.9

- External charge detection: voltage trend on 2-16 sample ring buffer (10s interval)
- Hysteresis state machine: up/down arrow + blinking battery icon on rise
- Vmax capture: when voltage stabilizes after charge plateau → persistent in EEPROM
- Separate thresholds: 1A for charge invalidation (compressor-safe)
- Various display improvements: Ah left / V right with trend arrow
- At-rest display: estimated capacity in bottom-left instead of time remaining
- Vcal saved only at end of calibration cycle (not periodic)
- Removed Info cal menu page (flash space reclaimed)

## v1.8

- Removed auto eco mode activation (manual toggle only)
- Removed capacityTrusted flag (calibration runs continuously, self-stabilizes)
- Changing nominal capacity now always resets estimated capacity
- Optimized parameters from genetic simulation:
  - First calibration step: 2 min (was 1 min)
  - Vmin/Vmax fast convergence: 0.04 (was 0.01)
  - Charge invalidation: 10s sustained >1A (was 5s)
  - SOC rest blend: 8% (was 5%)

## v1.7

- Unified weighted convergence: no more 30% threshold for first estimate
- Every calibration segment contributes, weighted by delta SOC (5%-50%)
- Capacity starts from nominal and converges with each rest-to-rest cycle
- capacityKnown renamed to capacityTrusted (trusted when estimate drifts >5% from nominal)
- Eco mode activates on trust, not on first big segment
- Simulation updated to match new calibration logic

## v1.6.1

- Separate charging detection threshold (1A) from rest threshold (0.3A)
- Prevents compressor cycling from invalidating calibration segments
- Added calibration simulation tool (sim/)

## v1.6

- Cell count configurable at runtime via menu (1-16S), stored in EEPROM
- SOC curve stored as per-cell values, auto-scaled by cell count
- Changing cell count resets Vmin/Vmax to new defaults
- Removed unused variables (menuTriggered, keyCalibrated)
- Initialized vbatMax/vbatMin at declaration (prevent theoretical div-by-zero)
- All remaining French comments translated to English
- Wiring diagram added to HARDWARE.md

## v1.5

- Voltage calibration (Vmin/Vmax) now gated behind 5s stable rest (same as capacity calibration)
- INA226 averaging increased from 4 to 16 samples (reduced noise)
- Deep sleep: first INA226 reading after wake-up discarded (bus restart noise)
- LFP_CELL_COUNT define: change one line to support 12V/24V/any cell count
- SOC curve defined per-cell and auto-scaled by cell count
- Sanity checks for Vmin/Vmax derived from cell count instead of hardcoded
- Last completed calibration cycle delta SOC% shown in Info cal

## v1.4

- INA226 current auto-zero: offset measured at stable rest, applied to all readings
- Current dead band (±50mA) to eliminate -0.0A display
- Charging invalidation requires 5s sustained current before resetting calibration segment
- Calibration counter visible at rest (hidden only when charging or eco mode)
- 3 visual states for calibration: blocked (blink), at rest (steady), accumulating (play triangle blinks)
- Removed Hardware diag page and Reset cal menu item (Reset ALL covers both)
- Info cal always shows estimated capacity (no more "--")
- Simplified menu: Capacity, Eco mode, Info cal, Reset ALL

## v1.3

Calibration accuracy improvements:

- Partial recharge now invalidates the current calibration segment (voltage hysteresis makes the SOC mapping unreliable after a charge event)
- Rest voltage requires 5s stable before being used as calibration endpoint (symmetric with segment start — LFP cells need settling time after load removal)
- SOC blend at rest reduced from 10% to 5% and gated behind 5s stable rest (prevents SOC jumps on the flat LFP curve where 10mV error = 5% SOC)
- Calibration target no longer consumed when segment has no valid start voltage (prevents losing accumulated coulombs)

## v1.2

- Calibration display reworked: 3 visual states
  - Blocked (needs rest): Ah blinks
  - At rest, segment valid: Ah steady
  - Accumulating: Ah steady + play triangle blinks
- Calibration counter stays visible at rest (hidden only when charging or eco mode)
- Rest voltage requires 5s stable before being accepted for calibration
- Long press center: 0.5s opens SYSTEM menu, 3s enters deep sleep
- Eco mode renamed from Sleep
- Flat SYSTEM menu: Capacity, Eco mode, Info cal, Reset cal, Hardware, Reset ALL
- Reset ALL wipes EEPROM and reboots
- Adaptive Ah decimals in Info cal (3 decimals below 1Ah)
- Menu titles auto-uppercased from item labels (no duplicate strings)
- Removed demo mode

## v1.1

- Replaced spinner with blinking calibration Ah counter
- Merged all diagnostics into a single Hardware screen (version, I2C, INA226 live, uptime)
- Removed unused diag pages (keypad test, screen test, individual I2C/INA/sysinfo)
- Flat SYSTEM menu with 3 entries: Battery, Hardware, Demo
- Demo mode now requires YES/NO confirmation
- Separated nominal capacity (user-set) from estimated capacity (calibration)
- Battery Info page showing calibration segment, voltage range, delta %
- Added version number display in Hardware diag
- Cleaned up dead code in BameGFX (drawText, drawMenu, drawProgress, drawFlowArrows, drawNumberXOR, drawCalSpinner)
- All comments and strings translated to English

## v1.0

- Initial release
- INA226 current/voltage sensing with coulomb counting
- Exponential doubling calibration for automatic Ah estimation
- SOC estimation: voltage lookup + coulomb counting + rest blending
- Menu system with editable values (inverse video selection, bracket editing)
- Battery settings: nominal capacity, reset calibration, auto deep sleep toggle
- Auto deep sleep activates when calibration reaches 30% delta SOC
- Voltage auto-calibration (Vmin/Vmax slow convergence at rest)
- OLED SSD1306 bicolor display with gauge bar and animations
- Deep sleep with adaptive wake interval based on voltage rate of change
- Foxeer Key23 keypad calibration at boot (5s hold)
- EEPROM persistence for all calibration data
