# Changelog

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
