#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <avr/wdt.h>
#if !BAME_DEV
  #include <avr/sleep.h>
  #include <avr/power.h>
#endif
#include <Adafruit_SSD1306.h>
#include <INA226.h>
#include "BameGFX.h"

// --- Configuration ---
#define BAME_VERSION "1.15"

#ifndef BAME_DEBUG
  #define BAME_DEBUG 0
#endif
#ifndef BAME_DEV
  #define BAME_DEV 0
#endif
// BAME_DEV=1 → dev build (nano): no sleep, no V min/max menu (flash-constrained)

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C

// Bicolor screen zones defined in BameGFX.h
#define INA226_ADDR 0x40

// Foxeer Key23 keypad on analog pin
#define KEY_PIN A3
// Action button (NO, pull to GND)
#define ACTION_BTN_PIN 2

// LiFePO4 battery
#define LFP_CELL_DEFAULT    4
#define LFP_CELL_FULL       3.65
#define LFP_CELL_EMPTY      2.50
#define BATTERY_CAPACITY_AH 80.0
#define SHUNT_RESISTANCE    0.0025
#define MAX_CURRENT         30.0

// Detection thresholds
#define VBAT_REST_CURRENT  0.3    // max current to consider at rest
#define VBAT_CHARGE_CURRENT 1.0  // min current to consider real charging
#define ACTIVE_CURRENT     0.5    // min |current| to show discharge/charge indicators
#define MIN_BATTERY_V      1.0    // min voltage to consider battery present
#define CAPACITY_MIN       1.0    // Ah sanity bounds
#define CAPACITY_MAX       500.0
#define LFP_CELL_CHARGE     3.40  // per-cell voltage above which external charge is suspected
#define LFP_CELL_CHARGE_MIN 3.375 // below this, a charger cannot be active
#define LFP_CELL_VMIN_UTILE 3.00  // default user practical min per cell
#define LFP_CELL_VMAX_UTILE 3.40  // default user practical max per cell (rest OCV at 100%)
#define VBAT_CONVERGE_FAST 0.04   // fast Vmin/Vmax convergence (~23 readings to move 1%, optimized from 0.01)
#define VBAT_CONVERGE_SLOW 0.001  // slow convergence (continuous adjustment)

// --- Voltage trend detection (linear regression over a ring buffer) ---
// The firmware samples voltage every VHIST_INTERVAL_MS into a VHIST_SIZE buffer,
// then computes a least-squares slope (in volts per sample step). The slope is
// compared against VHIST_SLOPE_THRESHOLD to decide up/down/flat.
//
// VHIST_SIZE = 8 samples × 10s = 80s observation window.
//   Tuned against sim/trend_sim.py on a realistic glaciere cycle
//   (13.30V idle → 13.20V load → log recovery to 13.30V over ~3 min).
//   16 samples (160s) averages the slope too much: a 100mV swing
//   produces only ±5mV/step and the trend never triggers. 8 samples
//   captures the same swing at ±8-12mV/step with ~20s detection latency.
//
// VHIST_SLOPE_THRESHOLD = 0.005 V/step = 0.5 mV/s average rate.
//   Sim shows a 100mV glaciere drop peaks at ~-10mV/step and the log
//   recovery peaks at ~+5mV/step — 0.005 catches both. A higher
//   threshold (0.010) misses the recovery; a lower one (0.003)
//   starts reacting to INA226 noise at rest.
//
// FLAT_COUNTDOWN_MS = 60s before the flat indicator vanishes.
//   Arbitrary, visible to the user — it's just the chrono shown on
//   screen while the arrow is flat; not used by the calibration gate.
#define VHIST_SIZE             8
#define VHIST_INTERVAL_MS      10000UL
#define VHIST_SLOPE_THRESHOLD  0.005f
#define FLAT_COUNTDOWN_MS      60000UL
// When voltageTrend flips to flat, scan the buffer backwards: count the newest
// samples that stay within FLAT_RETRO_SPREAD volts of each other, and backdate
// flatSince accordingly. Lets the chrono finish sooner when voltage was
// already quiet in the history.
#define FLAT_RETRO_SPREAD      0.020f

// Linear-regression denominators for x = 0..VHIST_SIZE-1 (compile-time folded).
#define VHIST_SX  ((VHIST_SIZE) * ((VHIST_SIZE) - 1) / 2)
#define VHIST_SXX ((VHIST_SIZE) * ((VHIST_SIZE) - 1) * (2 * (VHIST_SIZE) - 1) / 6)
#define VHIST_D   ((long)(VHIST_SIZE) * (VHIST_SXX) - (long)(VHIST_SX) * (VHIST_SX))

// EEPROM layout for voltage calibration (after keypad: addr 11+)
#define EEPROM_VCAL_MAGIC_ADDR 12
#define EEPROM_VCAL_ADDR       13  // 2 x float = 8 bytes (13-20)
#define EEPROM_VCAL_MAGIC_VAL  0xBB

// EEPROM layout for learned capacity (after vcal: addr 21+)
#define EEPROM_CAP_MAGIC_ADDR 21
#define EEPROM_CAP_ADDR       22  // 1 x float = 4 bytes (22-25)
#define EEPROM_CAP_MAGIC_VAL  0xCC

// EEPROM layout for nominal capacity (addr 26+)
#define EEPROM_NOM_MAGIC_ADDR 26
#define EEPROM_NOM_ADDR       27  // 1 x float = 4 bytes (27-30)
#define EEPROM_NOM_MAGIC_VAL  0xDD

// EEPROM layout for cell count (addr 31)
#define EEPROM_CELLS_ADDR 31  // 1 byte: cell count (0xFF = default)

// EEPROM layout for auto deep sleep
#define EEPROM_ASLEEP_ADDR        36  // 1 byte: 0x01 = active


// Cell count and dynamic voltages
uint8_t cellCount = LFP_CELL_DEFAULT;
float vbatTop = LFP_CELL_DEFAULT * LFP_CELL_FULL;    // calibration window upper bound
float vbatBottom = LFP_CELL_DEFAULT * LFP_CELL_EMPTY; // calibration window lower bound

// User practical operating range (set via menu, or hardcoded in dev build)
#ifndef BAME_VMIN_UTILE
  #define BAME_VMIN_UTILE (LFP_CELL_DEFAULT * LFP_CELL_VMIN_UTILE)
#endif
#ifndef BAME_VMAX_UTILE
  #define BAME_VMAX_UTILE (LFP_CELL_DEFAULT * LFP_CELL_VMAX_UTILE)
#endif
float vMinUtile = BAME_VMIN_UTILE;
float vMaxUtile = BAME_VMAX_UTILE;

// INA226 current offset auto-zero (measured at stable rest)
float currentOffset = 0;

// Battery capacity
float batteryCapacityNom = BATTERY_CAPACITY_AH;  // nominal (user)
float batteryCapacityAh = BATTERY_CAPACITY_AH;   // estimated (calibration or nominal fallback)
float batteryCapacityAs = BATTERY_CAPACITY_AH * 3600.0;

// Capacity calibration by exponential doubling
float calCoulombs = 0;          // accumulated coulombs for current segment
float calChargeSec = 0;         // seconds of sustained charging
float calTarget = 0;            // coulomb target (0 = initial time mode)
float calStartVoltage = 0;      // rest voltage at segment start
unsigned long calStartMs = 0;   // segment start timestamp
#if BAME_DEV
// Preempted segment end (dev-only, flash-heavy): flagged when target reached
// AND chrono just started. If chrono elapses, commit. If voltage moves first,
// rollback (extend segment).
float pendingEndVoltage = 0;
float pendingEndCoulombs = 0;   // 0 when no preempt pending
unsigned long pendingEndMs = 0;
#endif
#define CAL_INITIAL_TIME_MS  120000  // 2 min for first step (longer = bigger delta SOC = better first estimate)
#define CAL_MIN_COULOMBS     500.0   // ~0.14Ah minimum (INA226 precision)

#if !BAME_DEV
bool autoDeepSleep = false;   // auto deep sleep after inactivity timeout
#else
#define autoDeepSleep false
#endif
bool externalChargeDetected = false; // external charge detected (voltage rising unexpectedly)
int8_t voltageTrend = 0;             // slope sign: -1 down, 0 flat, +1 up
float bufMin = 0;                    // min voltage over sample buffer (0 until full)
// Smoothed current from linear regression on coulombCount snapshots.
// cAvg   : slope → average current over the VHIST_SIZE window (A). 0 until buffer full.
// maxSliceI : largest consecutive-sample charge-diff / 10s → max |I| over any 10s slice.
//             Used as the rest gate (catches cyclic loads that a mean would miss).
float cAvg = 0;
float maxSliceI = 0;
#if BAME_DEV
unsigned long flatSince = 0;         // last time arrow was up/down; drives calibration rest gate + display chrono
#endif

// --- Objects ---
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
BameGFX gfx(display);
INA226 ina(INA226_ADDR, &Wire);

// --- Global variables ---
float voltage = 0;
float current = 0;
float power = 0;
float socPercent = 100.0;
float coulombCount = 0;
// coulombRaw = pure continuous integrator, NEVER corrected by SOC blend.
// Used as the source for chist[] so cAvg is not polluted by blend jumps.
// Renormalized when it exceeds bounds (deltas against chist entries preserved).
float coulombRaw = 0;
unsigned long lastMeasure = 0;
unsigned long lastDisplay = 0;

#define MEASURE_INTERVAL_MS 100
#define DISPLAY_INTERVAL_MS 500

// ===========================================
// Buttons - Foxeer Key23
// ===========================================
enum Button { BTN_NONE, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_CENTER };

// Forward declarations
void waitButtonRelease();
Button readButton();
float socFromVoltage(float v);
void settingsMenu();
void batteryMenu();

#define MENU_PRESS_MS      500  // long press -> system menu
#if !BAME_DEV
// Sleep
#define DEEPSLEEP_PRESS_MS 3000 // longer press -> deep sleep
#define AUTO_SLEEP_MS    60000   // 60s no interaction -> screen sleep
#define AUTO_DEEPSLEEP_MS 300000 // 5min no interaction -> deep sleep
bool oledSleeping = false;
#else
#define oledSleeping false
#endif
unsigned long lastInteraction = 0;

#if !BAME_DEV
// ISR for deep sleep wake-up (INT0 on D2)
volatile bool wakeUpFlag = false;
void wakeUpISR() { wakeUpFlag = true; }

void enterOledSleep() {
  oledSleeping = true;
  display.ssd1306_command(SSD1306_DISPLAYOFF);
}

void exitOledSleep() {
  oledSleeping = false;
  display.ssd1306_command(SSD1306_DISPLAYON);
}

// Watchdog ISR for periodic wake-up
volatile bool wdtWakeUp = false;
ISR(WDT_vect) { wdtWakeUp = true; }

void enterDeepSleep() {
  #if BAME_DEBUG
  Serial.println(F("[SLEEP] Deep sleep..."));
  Serial.flush();
  #endif
  display.ssd1306_command(SSD1306_DISPLAYOFF);

  // Wake-up via D2 button
  attachInterrupt(digitalPinToInterrupt(ACTION_BTN_PIN), wakeUpISR, LOW);

  // Configure 8s watchdog for periodic wake-up (voltage measurement)
  cli();
  wdt_reset();
  MCUSR &= ~(1 << WDRF);
  WDTCSR |= (1 << WDCE) | (1 << WDE);
  WDTCSR = (1 << WDIE) | (1 << WDP3) | (1 << WDP0); // 8s, interrupt mode
  sei();

  // Adaptive interval: 5min (38 cycles) to 1h (450 cycles)
  // Faster voltage drop = more frequent measurements
  #define WDT_CYCLES_MIN  38   // 5 min
  #define WDT_CYCLES_MAX  450  // 1 heure
  uint16_t wdtTarget = WDT_CYCLES_MAX; // start slow
  uint16_t wdtCount = 0;
  float prevVoltage = voltage;

  while (!wakeUpFlag) {
    wdtWakeUp = false;
    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    sleep_enable();
    sleep_mode();
    sleep_disable();

    if (wdtWakeUp && !wakeUpFlag) {
      wdtCount++;
      if (wdtCount >= wdtTarget) {
        wdtCount = 0;

        // Measure (skip first reading after INA226 restart — noisy)
        Wire.begin();
        if (ina.begin()) {
          ina.setMaxCurrentShunt(MAX_CURRENT, SHUNT_RESISTANCE);
          ina.getBusVoltage(); // discard first reading
          ina.getCurrent();
          voltage = ina.getBusVoltage();
          current = ina.getCurrent();

          // Adapt interval based on voltage variation
          float deltaV = abs(voltage - prevVoltage);
          prevVoltage = voltage;

          if (deltaV > 0.5) {
            wdtTarget = WDT_CYCLES_MIN;        // fast drop -> 5min
          } else if (deltaV > 0.1) {
            wdtTarget = WDT_CYCLES_MIN * 3;    // ~15min
          } else if (deltaV > 0.02) {
            wdtTarget = WDT_CYCLES_MAX / 2;    // ~30min
          } else {
            wdtTarget = WDT_CYCLES_MAX;         // stable -> 1h
          }
        }
      }
    }
  }

  // --- Wake-up by button ---
  // Disable watchdog
  wdt_disable();
  detachInterrupt(digitalPinToInterrupt(ACTION_BTN_PIN));

  display.ssd1306_command(SSD1306_DISPLAYON);
  oledSleeping = false;
  wakeUpFlag = false;

  // Re-sync SOC from voltage on wake-up
  socPercent = socFromVoltage(voltage);
  coulombCount = (socPercent / 100.0) * batteryCapacityAs;

  #if BAME_DEBUG
  Serial.println(F("[SLEEP] Wake up"));
  #endif
  waitButtonRelease();
}
#endif // !BAME_DEV

// EEPROM layout for keypad calibration
#define EEPROM_MAGIC_ADDR 0        // 1 byte: 0xCA = calibrated
#define EEPROM_CAL_ADDR   1        // 5 x 2 bytes (int) = 10 bytes
#define EEPROM_MAGIC_VAL  0xCA
#define CAL_BTN_COUNT 5

// Order: CENTER, UP, DOWN, LEFT, RIGHT (sorted: 1, 416, 616, 748, 838)
// Gaps: 1-416=415, 416-616=200, 616-748=132, 748-838=90, 838-1023=185
// Tolerance = min(left gap, right gap) / 2
int keyCalVals[CAL_BTN_COUNT] = {838, 616, 1, 748, 416}; // defaults with 10k pullup


void saveCalToEEPROM() {
  EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VAL);
  for (int i = 0; i < CAL_BTN_COUNT; i++) {
    EEPROM.put(EEPROM_CAL_ADDR + i * 2, keyCalVals[i]);
  }
}

bool loadCalFromEEPROM() {
  if (EEPROM.read(EEPROM_MAGIC_ADDR) != EEPROM_MAGIC_VAL) return false;
  for (int i = 0; i < CAL_BTN_COUNT; i++) {
    EEPROM.get(EEPROM_CAL_ADDR + i * 2, keyCalVals[i]);
  }
  return true;
}

// Helpers: keep Ah and As in sync, reset calibration segment
inline void setCapacity(float ah) {
  batteryCapacityAh = ah;
  batteryCapacityAs = ah * 3600.0;
}
inline void resetCalibration() {
  calCoulombs = 0;
  calTarget = 0;
  calStartVoltage = 0;
#if BAME_DEV
  pendingEndCoulombs = 0;
#endif
}

// --- Voltage calibration EEPROM ---
void saveVcalToEEPROM() {
  EEPROM.write(EEPROM_VCAL_MAGIC_ADDR, EEPROM_VCAL_MAGIC_VAL);
  EEPROM.put(EEPROM_VCAL_ADDR, vbatTop);
  EEPROM.put(EEPROM_VCAL_ADDR + sizeof(float), vbatBottom);
}

bool loadVcalFromEEPROM() {
  if (EEPROM.read(EEPROM_VCAL_MAGIC_ADDR) != EEPROM_VCAL_MAGIC_VAL) return false;
  EEPROM.get(EEPROM_VCAL_ADDR, vbatTop);
  EEPROM.get(EEPROM_VCAL_ADDR + sizeof(float), vbatBottom);
  // Sanity check
  float vFull = cellCount * LFP_CELL_FULL;
  float vEmpty = cellCount * LFP_CELL_EMPTY;
  if (vbatTop < vEmpty || vbatTop > vFull * 1.4) vbatTop = vFull;
  if (vbatBottom < vEmpty * 0.5 || vbatBottom > vFull) vbatBottom = vEmpty;
  if (vbatBottom >= vbatTop) { vbatTop = vFull; vbatBottom = vEmpty; }
  return true;
}

// --- EEPROM helpers for float ---
void saveFloatEEPROM(uint8_t magicAddr, uint8_t magicVal, uint8_t dataAddr, float val) {
  EEPROM.write(magicAddr, magicVal);
  EEPROM.put(dataAddr, val);
}

bool loadFloatEEPROM(uint8_t magicAddr, uint8_t magicVal, uint8_t dataAddr, float &val) {
  if (EEPROM.read(magicAddr) != magicVal) return false;
  EEPROM.get(dataAddr, val);
  return true;
}

// Estimated capacity (calibration)
#define saveCapToEEPROM() saveFloatEEPROM(EEPROM_CAP_MAGIC_ADDR, EEPROM_CAP_MAGIC_VAL, EEPROM_CAP_ADDR, batteryCapacityAh)
bool loadCapFromEEPROM() {
  if (!loadFloatEEPROM(EEPROM_CAP_MAGIC_ADDR, EEPROM_CAP_MAGIC_VAL, EEPROM_CAP_ADDR, batteryCapacityAh)) return false;
  if (batteryCapacityAh < CAPACITY_MIN || batteryCapacityAh > CAPACITY_MAX) batteryCapacityAh = batteryCapacityNom;
  batteryCapacityAs = batteryCapacityAh * 3600.0;
  return true;
}
// Note: loadCapFromEEPROM reads directly into batteryCapacityAh via EEPROM.get

// Nominal capacity (user)
#define saveNomToEEPROM() saveFloatEEPROM(EEPROM_NOM_MAGIC_ADDR, EEPROM_NOM_MAGIC_VAL, EEPROM_NOM_ADDR, batteryCapacityNom)
bool loadNomFromEEPROM() {
  if (!loadFloatEEPROM(EEPROM_NOM_MAGIC_ADDR, EEPROM_NOM_MAGIC_VAL, EEPROM_NOM_ADDR, batteryCapacityNom)) return false;
  if (batteryCapacityNom < CAPACITY_MIN || batteryCapacityNom > CAPACITY_MAX) batteryCapacityNom = BATTERY_CAPACITY_AH;
  return true;
}

void resetCapEEPROM() {
  EEPROM.write(EEPROM_CAP_MAGIC_ADDR, 0xFF);
  setCapacity(batteryCapacityNom);
  resetCalibration();
  calStartMs = 0;
}

void saveCellsToEEPROM() { EEPROM.write(EEPROM_CELLS_ADDR, cellCount); }
void loadCellsFromEEPROM() {
  uint8_t v = EEPROM.read(EEPROM_CELLS_ADDR);
  if (v >= 1 && v <= 16) cellCount = v;
}

#if !BAME_DEV
void saveAutoSleepToEEPROM() { EEPROM.write(EEPROM_ASLEEP_ADDR, autoDeepSleep ? 0x01 : 0x00); }
void loadAutoSleepFromEEPROM() { autoDeepSleep = (EEPROM.read(EEPROM_ASLEEP_ADDR) == 0x01); }
#endif


// Thresholds computed dynamically from calibrated values
// Each button has its own tolerance = distance to nearest neighbor / 2
int keyThresholds[CAL_BTN_COUNT];

void computeThresholds() {
  // Sort values to find distances
  int sorted[CAL_BTN_COUNT + 1]; // +1 for NONE (1023)
  int sortIdx[CAL_BTN_COUNT];
  for (int i = 0; i < CAL_BTN_COUNT; i++) {
    sorted[i] = keyCalVals[i];
    sortIdx[i] = i;
  }
  sorted[CAL_BTN_COUNT] = 1023; // idle value

  // Simple sort of values
  for (int i = 0; i < CAL_BTN_COUNT; i++) {
    for (int j = i + 1; j < CAL_BTN_COUNT; j++) {
      if (sorted[j] < sorted[i]) {
        int tmp = sorted[i]; sorted[i] = sorted[j]; sorted[j] = tmp;
        tmp = sortIdx[i]; sortIdx[i] = sortIdx[j]; sortIdx[j] = tmp;
      }
    }
  }

  // Compute threshold for each button
  for (int i = 0; i < CAL_BTN_COUNT; i++) {
    int distLeft = (i == 0) ? sorted[i] : sorted[i] - sorted[i - 1];
    int distRight = sorted[i + 1] - sorted[i];
    int minDist = (distLeft < distRight) ? distLeft : distRight;
    keyThresholds[sortIdx[i]] = minDist / 2 - 1;
    if (keyThresholds[sortIdx[i]] < 10) keyThresholds[sortIdx[i]] = 10;
  }
}

Button readButton() {
  // Physical action button = CENTER
  if (digitalRead(ACTION_BTN_PIN) == LOW) return BTN_CENTER;

  // Foxeer pad
  int val = analogRead(KEY_PIN);
  if (abs(val - keyCalVals[0]) < keyThresholds[0]) return BTN_CENTER;
  if (abs(val - keyCalVals[1]) < keyThresholds[1]) return BTN_UP;
  if (abs(val - keyCalVals[2]) < keyThresholds[2]) return BTN_DOWN;
  if (abs(val - keyCalVals[3]) < keyThresholds[3]) return BTN_LEFT;
  if (abs(val - keyCalVals[4]) < keyThresholds[4]) return BTN_RIGHT;
  return BTN_NONE;
}

// Debounce: returns button if stable for 50ms
Button readButtonDebounced() {
  Button b = readButton();
  if (b == BTN_NONE) return BTN_NONE;
  delay(50);
  Button b2 = readButton();
  return (b == b2) ? b : BTN_NONE;
}

// Wait for button release
void waitButtonRelease() {
  while (readButton() != BTN_NONE) delay(10);
}

// Long press: 2 tiers — SYSTEM menu (1.5s) or deep sleep (3s)
void checkLongPress() {
  unsigned long pressStart = millis();
  while (readButton() == BTN_CENTER) {
    unsigned long held = millis() - pressStart;
#if !BAME_DEV
    if (held >= MENU_PRESS_MS) {
      display.clearDisplay();
      float pct = (float)(held - MENU_PRESS_MS) * 100.0f / (DEEPSLEEP_PRESS_MS - MENU_PRESS_MS);
      if (pct > 100.0f) pct = 100.0f;
      gfx.drawGauge(pct, F("SLEEP"));
      display.display();
    }
    if (held >= DEEPSLEEP_PRESS_MS) {
      waitButtonRelease();
      enterDeepSleep();
      lastMeasure = millis();
      lastInteraction = millis();
      return;
    }
#endif
    delay(50);
  }
  // Released — open menu if held past tier 1
  if (millis() - pressStart >= MENU_PRESS_MS) {
    waitButtonRelease();
    settingsMenu();
    lastInteraction = millis();
  }
}

// ===========================================
// SOC
// ===========================================
struct SocPoint {
  float voltage;
  float soc;
};

// Per-cell LFP SOC curve (chemistry-defined shape)
const SocPoint socCurve[] = {
  {3.65, 100}, {3.40,  99}, {3.35,  90}, {3.325, 70},
  {3.30,  40}, {3.275, 30}, {3.25,  20}, {3.20,  17},
  {3.00,  14}, {2.75,   9}, {2.50,   0},
};
const int socCurveSize = sizeof(socCurve) / sizeof(socCurve[0]);

float socFromVoltage(float v) {
  // Convert pack voltage to per-cell, rescale to reference curve
  float vCell = v / cellCount;
  float vMinCell = vbatBottom / cellCount;
  float vMaxCell = vbatTop / cellCount;
  float vScaled = LFP_CELL_EMPTY +
    (vCell - vMinCell) / (vMaxCell - vMinCell) *
    (LFP_CELL_FULL - LFP_CELL_EMPTY);

  if (vScaled >= socCurve[0].voltage) return 100.0;
  if (vScaled <= socCurve[socCurveSize - 1].voltage) return 0.0;
  for (int i = 0; i < socCurveSize - 1; i++) {
    if (vScaled >= socCurve[i + 1].voltage) {
      float ratio = (vScaled - socCurve[i + 1].voltage) / (socCurve[i].voltage - socCurve[i + 1].voltage);
      return socCurve[i + 1].soc + ratio * (socCurve[i].soc - socCurve[i + 1].soc);
    }
  }
  return 0.0;
}

// ===========================================
// Diagnostics
// ===========================================

// --- SYSTEM menu (flat) ---
enum MenuItem {
  ITEM_CAP,
  ITEM_CELLS,
#if !BAME_DEV
  ITEM_VMIN,
  ITEM_VMAX,
  ITEM_ECO,
#endif
  ITEM_RESET,
  ITEM_INFO_V,    // read-only: current voltage + min observed
  SYS_COUNT
};

void settingsMenu() {
  uint8_t sel = 0;
  int8_t editing = -1;

  float tmpCap = batteryCapacityNom;
  uint8_t tmpCells = cellCount;
#if !BAME_DEV
  float tmpVmin = vMinUtile;
  float tmpVmax = vMaxUtile;
#endif
  bool tmpConfirm = false;
#if !BAME_DEV
  bool tmpSleep = autoDeepSleep;
#endif

  while (true) {
    display.clearDisplay();
    gfx.drawTitle(F("Bame v" BAME_VERSION));

    // Info row (display only): current voltage + min observed
    {
      uint8_t y = BLUE_Y + ITEM_INFO_V * 8;
      display.setTextSize(1);
      if (sel == ITEM_INFO_V) {
        display.fillRect(0, y, SCREEN_W, 8, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
      } else {
        display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
      }
      display.setCursor(0, y);
      display.print(voltage, 2);
      display.print(F("V min:"));
      if (bufMin > 0) display.print(bufMin, 2);
      else display.print('-');
      display.setTextColor(SSD1306_WHITE);
    }

    char buf[10];

    // Capacity
    int capVal = (int)(editing == ITEM_CAP ? tmpCap : batteryCapacityNom);
    itoa(capVal, buf, 10);
    strcat(buf, "Ah");
    gfx.drawMenuItem(ITEM_CAP, ' ', F("Capacity"), buf, sel == ITEM_CAP, editing == ITEM_CAP);

    // Cells
    itoa(editing == ITEM_CELLS ? tmpCells : cellCount, buf, 10);
    strcat(buf, "S");
    gfx.drawMenuItem(ITEM_CELLS, ' ', F("Cells"), buf, sel == ITEM_CELLS, editing == ITEM_CELLS);

#if !BAME_DEV
    // V min utile (with observed min appended)
    dtostrf(editing == ITEM_VMIN ? tmpVmin : vMinUtile, 0, 1, buf);
    {
      char *p = buf + strlen(buf);
      *p++ = 'V';
      if (editing != ITEM_VMIN && bufMin > 0) {
        *p++ = '/';
        dtostrf(bufMin, 0, 1, p);
      } else *p = 0;
    }
    gfx.drawMenuItem(ITEM_VMIN, ' ', F("V min"), buf, sel == ITEM_VMIN, editing == ITEM_VMIN);

    // V max utile
    dtostrf(editing == ITEM_VMAX ? tmpVmax : vMaxUtile, 0, 1, buf);
    strcat(buf, "V");
    gfx.drawMenuItem(ITEM_VMAX, ' ', F("V max"), buf, sel == ITEM_VMAX, editing == ITEM_VMAX);
    // Eco mode
    gfx.drawMenuItem(ITEM_ECO, ' ', F("Eco mode"),
      (editing == ITEM_ECO ? tmpSleep : autoDeepSleep) ? "ON" : "OFF",
      sel == ITEM_ECO, editing == ITEM_ECO);
#endif

    // Reset ALL
    gfx.drawMenuItem(ITEM_RESET, ' ', F("Reset ALL"),
      editing == ITEM_RESET ? (tmpConfirm ? "YES" : "NO") : "",
      sel == ITEM_RESET, editing == ITEM_RESET);

    display.display();

    Button b = readButtonDebounced();
    if (b == BTN_NONE) { delay(50); continue; }

    if (editing >= 0) {
      switch (b) {
        case BTN_UP:
          if (editing == ITEM_CAP) { tmpCap += 1; if (tmpCap > CAPACITY_MAX) tmpCap = CAPACITY_MAX; }
          else if (editing == ITEM_CELLS) { tmpCells += 1; if (tmpCells > 16) tmpCells = 16; }
#if !BAME_DEV
          else if (editing == ITEM_VMIN) { tmpVmin += 0.05; if (tmpVmin > cellCount * LFP_CELL_CHARGE) tmpVmin = cellCount * LFP_CELL_CHARGE; }
          else if (editing == ITEM_VMAX) { tmpVmax += 0.05; if (tmpVmax > cellCount * LFP_CELL_FULL) tmpVmax = cellCount * LFP_CELL_FULL; }
#endif
#if !BAME_DEV
          else if (editing == ITEM_ECO) { tmpSleep = !tmpSleep; }
#endif
          else if (editing == ITEM_RESET) { tmpConfirm = !tmpConfirm; }
          break;
        case BTN_DOWN:
          if (editing == ITEM_CAP) { tmpCap -= 1; if (tmpCap < CAPACITY_MIN) tmpCap = CAPACITY_MIN; }
          else if (editing == ITEM_CELLS) { tmpCells -= 1; if (tmpCells < 1) tmpCells = 1; }
#if !BAME_DEV
          else if (editing == ITEM_VMIN) { tmpVmin -= 0.05; if (tmpVmin < cellCount * LFP_CELL_EMPTY) tmpVmin = cellCount * LFP_CELL_EMPTY; }
          else if (editing == ITEM_VMAX) { tmpVmax -= 0.05; if (tmpVmax < cellCount * LFP_CELL_CHARGE) tmpVmax = cellCount * LFP_CELL_CHARGE; }
#endif
#if !BAME_DEV
          else if (editing == ITEM_ECO) { tmpSleep = !tmpSleep; }
#endif
          else if (editing == ITEM_RESET) { tmpConfirm = !tmpConfirm; }
          break;
        case BTN_CENTER:
          if (editing == ITEM_CAP) {
            batteryCapacityNom = tmpCap;
            saveNomToEEPROM();
            setCapacity(batteryCapacityNom);
            saveCapToEEPROM();
          } else if (editing == ITEM_CELLS) {
            cellCount = tmpCells;
            saveCellsToEEPROM();
            vbatTop = cellCount * LFP_CELL_FULL;
            vbatBottom = cellCount * LFP_CELL_EMPTY;
            saveVcalToEEPROM();
          }
#if !BAME_DEV
          else if (editing == ITEM_VMIN) {
            vMinUtile = tmpVmin;
            // TODO: saveVutileToEEPROM();
          } else if (editing == ITEM_VMAX) {
            vMaxUtile = tmpVmax;
            // TODO: saveVutileToEEPROM();
          }
#endif
#if !BAME_DEV
          else if (editing == ITEM_ECO) {
            autoDeepSleep = tmpSleep;
            saveAutoSleepToEEPROM();
          }
#endif
          else if (editing == ITEM_RESET) {
            if (tmpConfirm) {
              for (uint16_t i = 0; i < E2END + 1; i++) EEPROM.write(i, 0xFF);
              wdt_enable(WDTO_15MS);
              while (true);
            }
          }
          editing = -1;
          break;
        case BTN_LEFT:
          tmpCap = batteryCapacityNom;
          tmpCells = cellCount;
#if !BAME_DEV
          tmpVmin = vMinUtile;
          tmpVmax = vMaxUtile;
          tmpSleep = autoDeepSleep;
#endif
          tmpConfirm = false;
          editing = -1;
          break;
        default: break;
      }
    } else {
      switch (b) {
        case BTN_UP: sel = (sel == 0) ? SYS_COUNT - 1 : sel - 1; break;
        case BTN_DOWN: sel = (sel + 1) % SYS_COUNT; break;
        case BTN_CENTER:
          if (sel == ITEM_INFO_V) break;  // info item: no action
          editing = sel;
          tmpCap = batteryCapacityNom;
          tmpCells = cellCount;
#if !BAME_DEV
          tmpVmin = vMinUtile;
          tmpVmax = vMaxUtile;
          tmpSleep = autoDeepSleep;
#endif
          tmpConfirm = false;
          break;
        case BTN_LEFT:
          waitButtonRelease();
          return;
        default: break;
      }
    }
    waitButtonRelease();
  }
}

// ===========================================
// Mode normal
// ===========================================

// --- Sensor readings ---
float readVoltage() { return ina.getBusVoltage(); }
float readCurrent() {
  float c = ina.getCurrent() - currentOffset;
  if (abs(c) < 0.05) c = 0;  // dead band to avoid -0.0 display
  return c;
}
float readPower() { return ina.getPower(); }

void updateMeasurements() {
  voltage = readVoltage();
  current = readCurrent();
  power = readPower();

  unsigned long now = millis();
  float dtSeconds = (now - lastMeasure) / 1000.0;
  lastMeasure = now;

  // No battery
  static bool batteryPresent = false;
  if (voltage < MIN_BATTERY_V) {
    batteryPresent = false;
    socPercent = 0;
    coulombCount = 0;
    return;
  }

  // 1) Init: wait for stable voltage before locking SOC
  if (!batteryPresent) {
    socPercent = socFromVoltage(voltage);
    coulombCount = (socPercent / 100.0) * batteryCapacityAs;
    if (voltage >= vbatBottom) {
      batteryPresent = true;
    }
    return;
  }

  // 2) Coulomb counting
  coulombCount -= current * dtSeconds;
  coulombRaw   -= current * dtSeconds;  // parallel, no clamp, no blend correction
  if (coulombCount > batteryCapacityAs) coulombCount = batteryCapacityAs;
  if (coulombCount < 0) coulombCount = 0;
  socPercent = (coulombCount / batteryCapacityAs) * 100.0;

  // 3) Exponential doubling calibration
  //    Accumulate discharge coulombs. A partial recharge invalidates
  //    the segment because the voltage-SOC mapping becomes unreliable
  //    after a charge event (surface charge, hysteresis).
  if (current > 0) {
    calCoulombs += current * dtSeconds;
    calChargeSec = 0;
  } else if (current < -VBAT_CHARGE_CURRENT) {
    // Invalidate only after sustained real charging (>1A for >5s)
    calChargeSec += dtSeconds;
    if (calChargeSec >= 10.0) { // 10s sustained >1A to invalidate
      resetCalibration();
      calChargeSec = 0;
    }
  } else {
    calChargeSec = 0;
  }

  // 4) Buffer analysis: voltage slope + smoothed current.
  //    Two parallel ring buffers sampled at the same 10s cadence:
  //    - vhist[]: voltage snapshots → slope → trend (up/down/flat)
  //    - chist[]: coulombCount snapshots → slope → average current (cAvg)
  //      AND consecutive-diffs → max current per 10s slice (maxSliceI, gate)
  //    Using coulombCount (continuously integrated at 100 ms) avoids the
  //    aliasing of snapshotting raw current on a cyclic load.
  {
    static float vhist[VHIST_SIZE];
    static float chist[VHIST_SIZE];  // coulombCount snapshots (A·s)
    static uint8_t vhistIdx = 0;
    static uint8_t vhistCount = 0;
    static unsigned long vhistLastMs = 0;

    if (vhistLastMs == 0 || now - vhistLastMs >= VHIST_INTERVAL_MS) {
      // Renormalize coulombRaw to keep float precision bounded: if it drifts
      // past ±100k A·s, subtract its value from all chist entries AND from
      // coulombRaw itself — deltas (what cAvg uses) are preserved.
      if (coulombRaw > 100000.0f || coulombRaw < -100000.0f) {
        float shift = coulombRaw;
        for (uint8_t i = 0; i < VHIST_SIZE; i++) chist[i] -= shift;
        coulombRaw = 0;
      }
      vhist[vhistIdx] = voltage;
      chist[vhistIdx] = coulombRaw;
      vhistIdx = (vhistIdx + 1) % VHIST_SIZE;
      if (vhistCount < VHIST_SIZE) vhistCount++;
      vhistLastMs = now;
    }

    if (vhistCount == VHIST_SIZE) {
      uint8_t startIdx = vhistIdx; // oldest sample (buffer full)
      float sumY = 0, sumIY = 0;
      bufMin = vhist[startIdx];
      float prevC = chist[startIdx];
      maxSliceI = 0;
      for (uint8_t i = 0; i < VHIST_SIZE; i++) {
        float v = vhist[(startIdx + i) % VHIST_SIZE];
        float c = chist[(startIdx + i) % VHIST_SIZE];
        sumY  += v;
        sumIY += (float)i * v;
        if (v < bufMin) bufMin = v;
        if (i > 0) {
          // |ΔcoulombCount| / 10s = |average current| over this 10s slice.
          float sliceI = fabs(c - prevC) / (float)(VHIST_INTERVAL_MS / 1000);
          if (sliceI > maxSliceI) maxSliceI = sliceI;
        }
        prevC = c;
      }
      // Voltage slope (V / 10s step), compile-time folded Sx/D.
      float slopeV = ((float)VHIST_SIZE * sumIY - (float)VHIST_SX * sumY) / (float)VHIST_D;
      voltageTrend = (slopeV > VHIST_SLOPE_THRESHOLD) ? 1
                   : (slopeV < -VHIST_SLOPE_THRESHOLD) ? -1 : 0;
      if (voltageTrend > 0) externalChargeDetected = true;
      else if (voltageTrend < 0) externalChargeDetected = false;
      // Smoothed current: sliding 2-point window against the LIVE coulombRaw.
      // cAvg refreshes every tick (100 ms), not only on sample push (10 s).
      // Using coulombRaw (not coulombCount) insulates cAvg from the SOC blend
      // that corrects coulombCount while stableRest is active.
      // Age of oldest sample = time since last push + (VHIST_SIZE-1) × interval.
      unsigned long ageMs = now - vhistLastMs
                          + (unsigned long)(VHIST_SIZE - 1) * VHIST_INTERVAL_MS;
      cAvg = -(coulombRaw - chist[startIdx]) * 1000.0f / (float)ageMs;
    }
    // Below 3.375V/cell a charger cannot be active (it imposes higher voltage)
    if (voltage < cellCount * LFP_CELL_CHARGE_MIN) externalChargeDetected = false;

    if (externalChargeDetected) resetCalibration();

#if BAME_DEV
    // Flat chrono (dev-only, flash-heavy): tracks how long the arrow has been
    // "flat" (neither up nor down). Once (now - flatSince) >= FLAT_COUNTDOWN_MS,
    // the rest gate opens (stableRest, see below). Two paths keep flatSince
    // up to date:
    //
    //  (a) Reset to now whenever the arrow is NOT flat, or the buffer is
    //      still warming up (bufMin == 0 means we haven't computed a slope
    //      yet, so "flat" is meaningless).
    //
    //  (b) Back-dating when the arrow IS flat: scan the sample buffer from
    //      the newest sample backwards, counting consecutive samples whose
    //      range (max - min) stays within FLAT_RETRO_SPREAD volts. That
    //      gives the length of the "quiet tail" already present in history.
    //      Range is used instead of std dev: on N <= 8 post-INA226-averaging
    //      samples the noise is near-gaussian and small, so range ≈ 4·σ.
    //      Only pulls flatSince earlier, never later, so a long steady
    //      chrono already in progress is not shortened.
    if (voltageTrend != 0 || bufMin == 0) {
      flatSince = now;
    } else if (vhistCount == VHIST_SIZE) {
      uint8_t newest = (vhistIdx + VHIST_SIZE - 1) % VHIST_SIZE;
      float tMin = vhist[newest];
      float tMax = tMin;
      uint8_t flatCount = 1;
      for (uint8_t back = 1; back < VHIST_SIZE; back++) {
        uint8_t idx = (newest + VHIST_SIZE - back) % VHIST_SIZE;
        float v = vhist[idx];
        if (v < tMin) tMin = v;
        if (v > tMax) tMax = v;
        if (tMax - tMin > FLAT_RETRO_SPREAD) break;
        flatCount++;
      }
      unsigned long backdated = now - (unsigned long)(flatCount - 1) * VHIST_INTERVAL_MS;
      if ((long)(backdated - flatSince) < 0) flatSince = backdated;
    }
#endif
  }

  // 5) Rest detection + segment save.
  // Dev build uses the flat chrono (optimistic open, preempt/commit/rollback).
  // Prod build uses a simpler direct gate to fit in flash.
#if BAME_DEV
  //   flatNow    = flat slope detected NOW (chrono started) → optimistic open
  //   stableRest = flat slope confirmed for the full chrono → SOC blend + save
  // Current gate: `maxSliceI < VBAT_REST_CURRENT` requires the current to have
  // stayed low across EVERY 10s slice of the 80s buffer — so cyclic loads
  // (compressor on/off) keep the gate shut even if `current` happens to be
  // zero at the instant we evaluate. `bufMin > 0` also implies buffer is full.
  bool flatNow = (bufMin > 0)
              && (maxSliceI < VBAT_REST_CURRENT)
              && (voltageTrend == 0);
  bool stableRest = flatNow && (now - flatSince >= FLAT_COUNTDOWN_MS);

  // Optimistic segment open on chrono start (don't wait 60s).
  if (flatNow && !externalChargeDetected && calStartVoltage == 0) {
    calStartVoltage = voltage;
    calStartMs = now;
  }
  // Discard optimistic open if flat reverted without any discharge flowing.
  if (voltageTrend != 0 && calStartVoltage > 0 && calCoulombs == 0
      && now - flatSince < FLAT_COUNTDOWN_MS) {
    calStartVoltage = 0;
  }
#else
  // Prod: direct gate — max current over any 10s slice low AND slope flat.
  bool stableRest = (bufMin > 0)
                 && (maxSliceI < VBAT_REST_CURRENT)
                 && (voltageTrend == 0);
#endif

  if (stableRest && !externalChargeDetected) {
    float socV = socFromVoltage(voltage);
    socPercent = socPercent * 0.92 + socV * 0.08;
    coulombCount = (socPercent / 100.0) * batteryCapacityAs;
    // Auto-zero current offset
    float raw = current + currentOffset;
    currentOffset = currentOffset * 0.9 + raw * 0.1;
#if !BAME_DEV
    // Prod: init segment start on first stable rest (dev does it optimistically above).
    if (calStartVoltage == 0) {
      calStartVoltage = voltage;
      calStartMs = now;
    }
#endif
    // Slow Vmax convergence toward observed rest voltage
    if (current >= -VBAT_REST_CURRENT && voltage > vbatTop * 0.98f) {
      float conv = (voltage > vbatTop) ? VBAT_CONVERGE_FAST : VBAT_CONVERGE_SLOW;
      vbatTop = vbatTop * (1.0f - conv) + voltage * conv;
    }
  }

  // Segment end gate: target reached?
  bool calTargetReached;
  if (calTarget <= 0) {
    calTargetReached = (now - calStartMs >= CAL_INITIAL_TIME_MS)
                    && (calCoulombs >= CAL_MIN_COULOMBS);
  } else {
    calTargetReached = (calCoulombs >= calTarget);
  }

#if BAME_DEV
  // --- Dev: preempt / commit / rollback ---
  // Preempt: target reached AND flat just detected → freeze candidate end.
  if (calTargetReached && calStartVoltage > 0 && calCoulombs > 0
      && flatNow && !externalChargeDetected
      && pendingEndCoulombs == 0) {
    pendingEndVoltage = voltage;
    pendingEndCoulombs = calCoulombs;
    pendingEndMs = now;
    calCoulombs = 0;
  }
  // Rollback: voltage moved before chrono elapsed → extend previous segment.
  if (pendingEndCoulombs > 0 && voltageTrend != 0
      && now - flatSince < FLAT_COUNTDOWN_MS) {
    calCoulombs += pendingEndCoulombs;
    pendingEndCoulombs = 0;
  }
  // Commit: chrono elapsed while pending → finalize segment with preempted end.
  if (pendingEndCoulombs > 0 && stableRest && !externalChargeDetected) {
    float socStart = socFromVoltage(calStartVoltage);
    float socEnd = socFromVoltage(pendingEndVoltage);
    float deltaSoc = socStart - socEnd;
    if (deltaSoc > 5.0) {
      float estAh = (pendingEndCoulombs / 3600.0) / (deltaSoc / 100.0);
      if (estAh > CAPACITY_MIN && estAh < CAPACITY_MAX) {
        float weight = constrain(deltaSoc / 100.0, 0.05, 0.5);
        setCapacity(batteryCapacityAh * (1.0 - weight) + estAh * weight);
        saveCapToEEPROM();
      }
    }
    saveVcalToEEPROM();
    calTarget = pendingEndCoulombs * 2.0;
    calStartVoltage = pendingEndVoltage;
    calStartMs = pendingEndMs;
    pendingEndCoulombs = 0;
  }
#else
  // --- Prod: single-shot save when target reached AND currently at rest ---
  if (calTargetReached && calStartVoltage > 0 && calCoulombs > 0 && stableRest) {
    float vEnd = voltage;
    float deltaSoc = socFromVoltage(calStartVoltage) - socFromVoltage(vEnd);
    if (deltaSoc > 5.0) {
      float estAh = (calCoulombs / 3600.0) / (deltaSoc / 100.0);
      if (estAh > CAPACITY_MIN && estAh < CAPACITY_MAX) {
        float weight = constrain(deltaSoc / 100.0, 0.05, 0.5);
        setCapacity(batteryCapacityAh * (1.0 - weight) + estAh * weight);
        saveCapToEEPROM();
      }
    }
    saveVcalToEEPROM();
    calTarget = calCoulombs * 2.0;
    calStartVoltage = vEnd;
    calStartMs = now;
    calCoulombs = 0;
  }
#endif
}

void updateDisplay() {
  display.clearDisplay();

  // No battery detected
  if (voltage < MIN_BATTERY_V) {
    gfx.drawGauge(0);
    display.setTextSize(2);
    display.setCursor(4, BLUE_Y + 12);
    if ((millis() / 500) % 2) display.print(F("No Battery"));
    display.display();
    return;
  }

  // === YELLOW ZONE (0-15): SOC gauge with XOR sprites ===
  gfx.drawGauge(socPercent);

  // === BLUE ZONE (16-63) ===

  // Line 1: Remaining Ah (left) + Voltage (right)
  int ahInt = (int)(coulombCount / 3600.0);
  uint8_t ahDigits = (ahInt >= 100) ? 3 : (ahInt >= 10) ? 2 : 1;
  // Size 2 pass: both big numbers
  display.setTextSize(2);
  display.setCursor(0, BLUE_Y + 2);
  display.print(ahInt);
  display.setCursor(SCREEN_W - 54, BLUE_Y + 2);
  display.print(voltage, 1);
  // Everything after this is size 1 (suffixes, countdown, line 2, line 3).
  display.setTextSize(1);
  display.setCursor(ahDigits * 12, BLUE_Y + 2);
  display.print(F("Ah"));
  display.setCursor(SCREEN_W - 6, BLUE_Y + 2);
  display.print(F("V"));
  // Voltage trend arrow (left of voltage). In dev, also shows 60→0 chrono
  // countdown while flat (flatSince is maintained by updateMeasurements).
  {
    int16_t ax = SCREEN_W - 66;
    int16_t ay = BLUE_Y + 6;
    if (voltageTrend > 0)
      display.fillTriangle(ax, ay + 6, ax + 4, ay, ax + 8, ay + 6, SSD1306_WHITE);
    else if (voltageTrend < 0)
      display.fillTriangle(ax, ay, ax + 8, ay, ax + 4, ay + 6, SSD1306_WHITE);
#if BAME_DEV
    else if (bufMin > 0) {
      uint32_t elapsed_ms = millis() - flatSince;
      if (elapsed_ms < FLAT_COUNTDOWN_MS) {
        uint8_t remaining = (FLAT_COUNTDOWN_MS / 1000) - (uint8_t)(elapsed_ms / 1000);
        display.setCursor(remaining < 10 ? ax + 2 : ax - 4, ay);
        display.print(remaining);
      }
    }
#endif
  }

  // Line 2: Power (smoothed when buffer full) + instantaneous current
  // Power uses cAvg × voltage once we have it — averages out cyclic loads.
  // Current shown as-is (raw A for live feedback).
  float iForPower = (bufMin > 0) ? cAvg : current;
  display.setCursor(0, BLUE_Y + 22);
  display.print((int)abs(iForPower * voltage));
  display.print(F("W"));
  // Amps right-aligned
  {
    int ci = (int)abs(current);
    uint8_t alen = 4; // "X.XA" minimum
    if (ci >= 10) alen++;
    if (ci >= 100) alen++;
    if (current < 0) alen++;
    display.setCursor(SCREEN_W - alen * 6, BLUE_Y + 22);
    display.print(current, 1);
    display.print(F("A"));
  }

  // Line 3: Arrow + time remaining
  // Use smoothed cAvg when buffer full — cyclic loads average out, autonomy
  // stops oscillating between "hours" and "infinity" as compressor cycles.
  int16_t ty = BLUE_Y + 37;
  float iForAutonomy = (bufMin > 0) ? cAvg : current;
  float hoursLeft = 0;
  if (iForAutonomy > ACTIVE_CURRENT) {
    hoursLeft = (coulombCount / 3600.0) / iForAutonomy;
  } else if (iForAutonomy < -ACTIVE_CURRENT) {
    float remaining = (batteryCapacityAs - coulombCount) / 3600.0;
    if (remaining < 0) remaining = 0;
    hoursLeft = remaining / (-iForAutonomy);
  }

  // Bottom left line: either HH:MM (active current) or capacity (at rest)
  if (abs(current) > ACTIVE_CURRENT) {
    hoursLeft = constrain(hoursLeft, 0.0f, 99.9f);
    int h = (int)hoursLeft;
    int m = (int)((hoursLeft - h) * 60);
    // Triangle: < discharge, > charge
    if (current > 0) display.fillTriangle(0, ty + 3, 6, ty, 6, ty + 6, SSD1306_WHITE);
    else             display.fillTriangle(6, ty + 3, 0, ty, 0, ty + 6, SSD1306_WHITE);
    display.setCursor(10, ty);
    if (h < 10) display.print('0');
    display.print(h);
    display.print(':');
    if (m < 10) display.print('0');
    display.print(m);
  } else if (!autoDeepSleep) {
    // At rest: show estimated capacity
    display.setCursor(0, ty);
    display.print((int)batteryCapacityAh);
    display.print(F("Ah"));
  }

#if BAME_DEV
  // Calibration counter (dev only, flash-heavy): blinks waiting for stable rest.
  if (!autoDeepSleep) {
    bool needsRest = (calStartVoltage == 0)
      || ((calTarget > 0) ? (calCoulombs >= calTarget)
      : ((millis() - calStartMs >= CAL_INITIAL_TIME_MS) && (calCoulombs >= CAL_MIN_COULOMBS)));
    bool blink = (millis() / DISPLAY_INTERVAL_MS) % 2;
    if (!needsRest || blink) {
      display.setCursor(50, ty);
      float calAh = calCoulombs / 3600.0;
      if (calAh >= 10.0) display.print((int)calAh);
      else display.print(calAh, 1);
      display.print(F("Ah"));
    }
  }
#endif

  // Bottom right: battery icon — charging (partial, blinking) or full (static)
  if (externalChargeDetected) {
    if ((millis() / DISPLAY_INTERVAL_MS) % 2)
      gfx.drawChargingBattery(106, ty, false);  // partial, blinks
  } else if (voltage >= cellCount * LFP_CELL_CHARGE) {
    gfx.drawChargingBattery(106, ty, true);     // full, static
  }

  display.display();
}

#if BAME_DEBUG
void debugSerial() {
  Serial.print(voltage, 2);
  Serial.print(F(" V | "));
  Serial.print(current, 2);
  Serial.print(F(" A | "));
  Serial.print(power, 1);
  Serial.print(F(" W | SOC: "));
  Serial.print(socPercent, 1);
  Serial.print(F("% | Coulombs: "));
  Serial.print(coulombCount, 0);
  Serial.print(F("/"));
  Serial.print(batteryCapacityAs, 0);
  if (current > ACTIVE_CURRENT) {
    float hoursLeft = (coulombCount / 3600.0) / current;
    Serial.print(F(" | Left: "));
    Serial.print(hoursLeft, 1);
    Serial.print(F("h"));
  }
  Serial.println();
}
#endif

// ===========================================
// Setup & Loop
// ===========================================

void setup() {
  #if BAME_DEBUG
  Serial.begin(115200);
  delay(500);
  #endif
  Wire.begin();
  pinMode(KEY_PIN, INPUT);
  pinMode(ACTION_BTN_PIN, INPUT_PULLUP);
  computeThresholds(); // default thresholds

  #if BAME_DEBUG
  Serial.println(F(""));
  Serial.println(F("========================="));
  Serial.println(F(" BaMe Battery Meter"));
  Serial.println(F(" LFP " BAME_VERSION));
  Serial.println(F("========================="));

  // Scan I2C
  Serial.println(F("[I2C] Scan..."));
  {
    int found = 0;
    for (byte addr = 1; addr < 127; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0) {
        Serial.print(F("  0x"));
        if (addr < 16) Serial.print(F("0"));
        Serial.println(addr, HEX);
        found++;
      }
    }
    Serial.print(found);
    Serial.println(F(" device(s)"));
  }
  #endif

  // Init OLED
  #if BAME_DEBUG
  Serial.println(F("[OLED] Init..."));
  #endif
  bool oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  #if BAME_DEBUG
  if (!oledOk) {
    Serial.println(F("[OLED] ERROR - not found"));
  } else {
    Serial.println(F("[OLED] OK"));
  }
  #endif
  if (oledOk) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
  }

  // Load keypad calibration from EEPROM
  const char* btnNames[] = {"CENTER", "UP", "DOWN", "LEFT", "RIGHT"};
  loadCalFromEEPROM();
  computeThresholds();

  // Button held at boot -> diag/calibration mode
  delay(200);
  if (analogRead(KEY_PIN) < 1000) {
    #define BOOT_CAL_MS 5000
    unsigned long pressStart = millis();
    bool calibrate = false;

    // Gauge 0->100% over 5s
    while (analogRead(KEY_PIN) < 1000) {
      unsigned long held = millis() - pressStart;
      float pct = (float)held * 100.0f / BOOT_CAL_MS;
      if (pct > 100.0f) pct = 100.0f;

      if (oledOk) {
        display.clearDisplay();
        gfx.drawGauge(pct, F("CAL"));
        display.setTextSize(1);
        display.setCursor(16, BLUE_Y + 9);
        display.print(F("Hold = calibrate"));
        display.display();
      }

      if (held >= BOOT_CAL_MS) {
        calibrate = true;
        break;
      }
      delay(50);
    }
    waitButtonRelease();

    if (calibrate) {
      // Keypad calibration
      delay(300);
      for (int b = 0; b < CAL_BTN_COUNT; b++) {
        int stableMin = 1023;
        bool pressed = false;
        bool done = false;

        while (!done) {
          int val = analogRead(KEY_PIN);
          if (val < 1000) {
            if (val < stableMin) stableMin = val;
            pressed = true;
          } else if (pressed) {
            keyCalVals[b] = stableMin;
            done = true;
          }

          if (oledOk) {
            display.clearDisplay();
            gfx.drawTitle(F("CALIBRATION"));
            display.setTextSize(2);
            display.setCursor(0, BLUE_Y + 2);
            display.println(btnNames[b]);
            display.setTextSize(1);
            display.setCursor(0, BLUE_Y + 22);
            display.print(F("ADC: "));
            display.print(val);
            if (pressed) {
              display.print(F("  min:"));
              display.print(stableMin);
            }
            display.display();
          }
          delay(50);
        }
        delay(300);
      }

      saveCalToEEPROM();
      computeThresholds();

      if (oledOk) {
        display.clearDisplay();
        gfx.drawTitle(F("CAL OK"));
        display.setTextSize(1);
        display.setCursor(0, BLUE_Y + 2);
        for (int i = 0; i < CAL_BTN_COUNT; i++) {
          display.print(btnNames[i]);
          display.print(F(": "));
          display.println(keyCalVals[i]);
        }
        display.display();
        delay(2000);
      }
    } else {
      // Short press -> settings menu
      settingsMenu();
    }
  }

  // Load cell count first (affects voltage defaults)
  loadCellsFromEEPROM();
  vbatTop = cellCount * LFP_CELL_FULL;
  vbatBottom = cellCount * LFP_CELL_EMPTY;

  // Load voltage calibration from EEPROM
  if (loadVcalFromEEPROM()) {
    #if BAME_DEBUG
    Serial.println(F("[VCAL] EEPROM loaded"));
    Serial.print(F("  Vmax=")); Serial.print(vbatTop, 2);
    Serial.print(F("  Vmin=")); Serial.println(vbatBottom, 2);
    #endif
  } else {
    #if BAME_DEBUG
    Serial.println(F("[VCAL] Using defaults"));
    #endif
  }

  // Load learned capacity
  loadNomFromEEPROM();  // nominal first (fallback)
  if (!loadCapFromEEPROM()) {
    // No calibration -> fallback to nominal
    setCapacity(batteryCapacityNom);
  }
#if !BAME_DEV
  loadAutoSleepFromEEPROM();
#endif
  // calCoulombs resets to 0 on each boot (fresh segment)

  // Init INA226
  bool inaOk = ina.begin();
  if (!inaOk) {
    #if BAME_DEBUG
    Serial.println(F("[INA226] ERROR - not found"));
    #endif
  } else {
    #if BAME_DEBUG
    Serial.println(F("[INA226] OK"));
    #endif
    ina.setMaxCurrentShunt(MAX_CURRENT, SHUNT_RESISTANCE);
    ina.setAverage(16);
  }

  // SOC initial
  delay(500);
  if (inaOk) {
    voltage = ina.getBusVoltage();
    socPercent = socFromVoltage(voltage);
    coulombCount = (socPercent / 100.0) * batteryCapacityAs;
  }

  #if BAME_DEBUG
  Serial.println(F("--- Start ---"));
  #endif
  lastMeasure = millis();
  lastInteraction = millis();
}

void loop() {
  unsigned long now = millis();

  // Button handling
  Button b = readButton();
  if (b != BTN_NONE) {
#if !BAME_DEV
    if (oledSleeping) {
      waitButtonRelease();
      exitOledSleep();
    } else
#endif
    if (b == BTN_CENTER) {
      checkLongPress();
    }
    lastInteraction = millis();
  }

#if !BAME_DEV
  // Screen auto-sleep after 60s
  if (!oledSleeping && (millis() - lastInteraction >= AUTO_SLEEP_MS)) {
    enterOledSleep();
  }
  // Auto deep sleep after 5min
  if (autoDeepSleep && (millis() - lastInteraction >= AUTO_DEEPSLEEP_MS)) {
    enterDeepSleep();
    lastMeasure = millis();
    lastInteraction = millis();
  }
#endif

  if (now - lastMeasure >= MEASURE_INTERVAL_MS) {
    updateMeasurements();
  }

  if (!oledSleeping && now - lastDisplay >= DISPLAY_INTERVAL_MS) {
    lastDisplay = now;
    gfx.tick();
    updateDisplay();
    #if BAME_DEBUG
    debugSerial();
    #endif
  }
}
