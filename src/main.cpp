// ===========================================================================
// BaMe v2 — pure coulomb counting
//
// Voltage is a display value plus a trigger for two events:
//   - Charger disconnect at top voltage → "battery full", SOC reset to 100%
//   - Voltage collapse → BMS cutoff, Ah delivered since last full = capacity
//
// In BUS install (every current passes the shunt) coulomb counting is
// bidirectional and partial charges are tracked accurately. In LOAD install
// the charger is invisible to the shunt, so an unexplained voltage rise
// flags `socUncertain` until the next full event clears it.
// ===========================================================================

#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <avr/wdt.h>
#include <Adafruit_SSD1306.h>
#include <INA226.h>
#include "BameGFX.h"
#include "bame_state.h"
#include "display.h"
#include "menu.h"

#define BAME_VERSION "2.0-wip"

#ifndef BAME_DEBUG
  #define BAME_DEBUG 0
#endif
#ifndef BAME_DEV
  #define BAME_DEV 0
#endif
#ifndef BAME_CELLS
  #define BAME_CELLS 4
#endif
#ifndef BAME_WIRING_BUS
  #define BAME_WIRING_BUS 1
#endif

// --- Hardware pins / I2C ---
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_ADDR       0x3C
#define INA226_ADDR     0x40
#define KEY_PIN         A3
#define ACTION_BTN_PIN  2

// --- INA226 ---
#define SHUNT_RESISTANCE 0.0025f
#define MAX_CURRENT      30.0f

// --- LFP physics ---
#define LFP_CELL_FULL          3.65f
#define LFP_CELL_TOP_REST      3.40f   // OCV plateau for "full" detection
#define LFP_CELL_BMS_NEAR      2.50f

// --- User defaults ---
#define BATTERY_CAPACITY_AH    80.0f
#define CAPACITY_MIN           1.0f
#define CAPACITY_MAX           500.0f

// --- Detection thresholds ---
#define MIN_BATTERY_V          1.0f    // BMS cut → voltage collapses near 0
#define VBAT_REST_CURRENT      0.3f
#define ACTIVE_CURRENT         0.5f
#define V_FULL_PER_CELL        LFP_CELL_TOP_REST
#define FULL_REST_MS           30000UL // sustained "at top + low I" for full event

// --- Smoothing / pacing ---
#define MEASURE_INTERVAL_MS    100
#define DISPLAY_INTERVAL_MS    500
#define CAVG_EWMA_TAU_S        30.0f
#define CAVG_EWMA_ALPHA        (MEASURE_INTERVAL_MS / 1000.0f / CAVG_EWMA_TAU_S)
#define EEPROM_SAVE_INTERVAL_MS 300000UL  // save coulombCount every 5 min

// --- EEPROM layout ---
#define EEPROM_KEYPAD_MAGIC_ADDR 0
#define EEPROM_KEYPAD_VAL        0xCA
#define EEPROM_KEYPAD_DATA_ADDR  1       // 5 × int = 10B
#define EEPROM_NOM_MAGIC_ADDR    11
#define EEPROM_NOM_VAL           0xDD
#define EEPROM_NOM_DATA_ADDR     12      // float
#define EEPROM_LEARN_MAGIC_ADDR  16
#define EEPROM_LEARN_VAL         0xEE
#define EEPROM_LEARN_DATA_ADDR   17      // float
#define EEPROM_COULOMB_MAGIC_ADDR 21
#define EEPROM_COULOMB_VAL       0xCC
#define EEPROM_COULOMB_DATA_ADDR 22      // float

#define CAL_BTN_COUNT 5

// ===========================================================================
// Hardware objects (definitions for externs in bame_state.h)
// ===========================================================================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
BameGFX gfx(display);
INA226 ina(INA226_ADDR, &Wire);

extern const uint8_t cellCount = BAME_CELLS;

// --- State definitions ---
float batteryCapacityNom = BATTERY_CAPACITY_AH;
float batteryCapacityAh  = BATTERY_CAPACITY_AH;
bool  capacityLearned    = false;

float voltage       = 0;
float current       = 0;
float power         = 0;
float currentOffset = 0;
float cAvg          = 0;
bool  cAvgInit      = false;

float coulombCount  = 0;
bool  socUncertain  = false;

bool  batteryPresent = false;
float coulombsAtLastFull       = 0;
unsigned long sinceLastFullMs  = 0;

unsigned long lastMeasure = 0;
unsigned long lastDisplay = 0;

// Local-only state (not exposed via bame_state.h)
static unsigned long restAtTopSinceMs = 0;  // when current first stayed low at high V
static unsigned long lastEepromSaveMs = 0;
static float vSlowAvg = 0;                  // slow-tracking voltage for LOAD partial-charge detection

// ===========================================================================
// Buttons (Foxeer Key23 keypad + physical action button)
// ===========================================================================
static int keyCalVals[CAL_BTN_COUNT] = {838, 616, 1, 748, 416};
static int keyThresholds[CAL_BTN_COUNT];

static void saveKeypadEEPROM() {
  EEPROM.write(EEPROM_KEYPAD_MAGIC_ADDR, EEPROM_KEYPAD_VAL);
  for (int i = 0; i < CAL_BTN_COUNT; i++) {
    EEPROM.put(EEPROM_KEYPAD_DATA_ADDR + i * 2, keyCalVals[i]);
  }
}
static bool loadKeypadEEPROM() {
  if (EEPROM.read(EEPROM_KEYPAD_MAGIC_ADDR) != EEPROM_KEYPAD_VAL) return false;
  for (int i = 0; i < CAL_BTN_COUNT; i++) {
    EEPROM.get(EEPROM_KEYPAD_DATA_ADDR + i * 2, keyCalVals[i]);
  }
  return true;
}

static void computeKeypadThresholds() {
  int sorted[CAL_BTN_COUNT + 1];
  int sortIdx[CAL_BTN_COUNT];
  for (int i = 0; i < CAL_BTN_COUNT; i++) { sorted[i] = keyCalVals[i]; sortIdx[i] = i; }
  sorted[CAL_BTN_COUNT] = 1023;
  for (int i = 0; i < CAL_BTN_COUNT; i++) {
    for (int j = i + 1; j < CAL_BTN_COUNT; j++) {
      if (sorted[j] < sorted[i]) {
        int t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t;
        t = sortIdx[i]; sortIdx[i] = sortIdx[j]; sortIdx[j] = t;
      }
    }
  }
  for (int i = 0; i < CAL_BTN_COUNT; i++) {
    int distLeft  = (i == 0) ? sorted[i] : sorted[i] - sorted[i - 1];
    int distRight = sorted[i + 1] - sorted[i];
    int minDist   = (distLeft < distRight) ? distLeft : distRight;
    keyThresholds[sortIdx[i]] = minDist / 2 - 1;
    if (keyThresholds[sortIdx[i]] < 10) keyThresholds[sortIdx[i]] = 10;
  }
}

Button readButton() {
  if (digitalRead(ACTION_BTN_PIN) == LOW) return BTN_CENTER;
  int val = analogRead(KEY_PIN);
  if (abs(val - keyCalVals[0]) < keyThresholds[0]) return BTN_CENTER;
  if (abs(val - keyCalVals[1]) < keyThresholds[1]) return BTN_UP;
  if (abs(val - keyCalVals[2]) < keyThresholds[2]) return BTN_DOWN;
  if (abs(val - keyCalVals[3]) < keyThresholds[3]) return BTN_LEFT;
  if (abs(val - keyCalVals[4]) < keyThresholds[4]) return BTN_RIGHT;
  return BTN_NONE;
}

Button readButtonDebounced() {
  Button b = readButton();
  if (b == BTN_NONE) return BTN_NONE;
  delay(50);
  Button b2 = readButton();
  return (b == b2) ? b : BTN_NONE;
}

void waitButtonRelease() {
  while (readButton() != BTN_NONE) delay(10);
}

// ===========================================================================
// EEPROM persistence
// ===========================================================================
static void saveFloatEEPROM(uint8_t magicAddr, uint8_t magicVal, uint8_t dataAddr, float val) {
  EEPROM.write(magicAddr, magicVal);
  EEPROM.put(dataAddr, val);
}
static bool loadFloatEEPROM(uint8_t magicAddr, uint8_t magicVal, uint8_t dataAddr, float &val) {
  if (EEPROM.read(magicAddr) != magicVal) return false;
  EEPROM.get(dataAddr, val);
  return true;
}

void saveNomEEPROM() {
  saveFloatEEPROM(EEPROM_NOM_MAGIC_ADDR, EEPROM_NOM_VAL, EEPROM_NOM_DATA_ADDR, batteryCapacityNom);
}
void saveLearnedEEPROM() {
  saveFloatEEPROM(EEPROM_LEARN_MAGIC_ADDR, EEPROM_LEARN_VAL, EEPROM_LEARN_DATA_ADDR, batteryCapacityAh);
}
void saveCoulombEEPROM() {
  saveFloatEEPROM(EEPROM_COULOMB_MAGIC_ADDR, EEPROM_COULOMB_VAL, EEPROM_COULOMB_DATA_ADDR, coulombCount);
}
void resetAllEEPROM() {
  for (uint16_t i = 0; i < E2END + 1; i++) EEPROM.write(i, 0xFF);
}

// ===========================================================================
// Helpers
// ===========================================================================
void setCapacityNom(float ah) {
  batteryCapacityNom = ah;
  if (!capacityLearned) batteryCapacityAh = ah;
}

// "Battery full" event handler. Called by both the auto-detector below and
// the manual menu action.
void declareBatteryFull(unsigned long now) {
  coulombCount = capacityAs();
  coulombsAtLastFull = coulombCount;
  sinceLastFullMs = now;
  socUncertain = false;
  saveCoulombEEPROM();
  saveLearnedEEPROM();
}

// "BMS cutoff" event handler. The Ah delivered since the last full event IS
// the measured capacity for this discharge cycle. Blend into learned cap.
static void declareBMSCutoff(unsigned long now) {
  if (sinceLastFullMs == 0) return;  // never had a full reference, can't measure
  float deliveredC = coulombsAtLastFull - coulombCount;
  if (deliveredC <= 0) return;
  float deliveredAh = deliveredC / 3600.0f;
  if (deliveredAh < CAPACITY_MIN || deliveredAh > CAPACITY_MAX) return;
  if (!capacityLearned) {
    batteryCapacityAh = deliveredAh;
    capacityLearned = true;
  } else {
    // 30% blend toward the new sample → converges over a few cycles
    batteryCapacityAh = batteryCapacityAh * 0.70f + deliveredAh * 0.30f;
  }
  saveLearnedEEPROM();
  coulombCount = 0;
  sinceLastFullMs = 0;
}

// ===========================================================================
// Sensor reads
// ===========================================================================
static float readVoltage() { return ina.getBusVoltage(); }
static float readCurrent() {
  float c = ina.getCurrent() - currentOffset;
  if (abs(c) < 0.05) c = 0;       // dead band against ±0.0 jitter
  return c;
}

// ===========================================================================
// Measurement loop — one tick (every MEASURE_INTERVAL_MS).
// ===========================================================================
static void updateMeasurements() {
  voltage = readVoltage();
  current = readCurrent();
  power   = ina.getPower();

  // EWMA on the smoothed current (display only)
  if (!cAvgInit) { cAvg = current; cAvgInit = true; }
  else           { cAvg = CAVG_EWMA_ALPHA * current + (1.0f - CAVG_EWMA_ALPHA) * cAvg; }

  unsigned long now = millis();
  float dtSeconds = (now - lastMeasure) / 1000.0f;
  if (dtSeconds <= 0 || dtSeconds > 1.0f) dtSeconds = 0;
  lastMeasure = now;

  // BMS cutoff: voltage collapse → battery is silent. Record the cycle.
  if (voltage < MIN_BATTERY_V) {
    if (batteryPresent) {
      declareBMSCutoff(now);
      batteryPresent = false;
    }
    return;
  }
  if (!batteryPresent) {
    // (Re)connection: trust whatever was in EEPROM. If the value is clearly
    // bogus, fall back to "assume nominal full" and flag uncertainty.
    if (coulombCount <= 0 || coulombCount > capacityAs() * 1.10f) {
      coulombCount = capacityAs();
      socUncertain = true;
    }
    batteryPresent = true;
    vSlowAvg = voltage;
  }

  // Coulomb integration. Negative current (charge in BUS install) increases
  // the count. Allow modest over/under-shoot so a cycle measurement at BMS
  // cutoff sees the real delivered Ah.
  coulombCount -= current * dtSeconds;
  float capAs = capacityAs();
  if (coulombCount > capAs * 1.10f) coulombCount = capAs * 1.10f;
  if (coulombCount < -capAs * 0.10f) coulombCount = -capAs * 0.10f;

  // Slow auto-zero of the current offset whenever |I| is low. Keeps a small
  // sensor bias from accumulating into the integrator over days.
  if (abs(current) < VBAT_REST_CURRENT) {
    float raw = current + currentOffset;
    currentOffset = currentOffset * 0.99f + raw * 0.01f;
  }

  // --- Event: "battery full" (works for both BUS and LOAD wirings) ---
  // Voltage at top, current near zero, sustained for FULL_REST_MS.
  bool atTop = (voltage / cellCount) >= V_FULL_PER_CELL
            && abs(current) < VBAT_REST_CURRENT;
  if (atTop) {
    if (restAtTopSinceMs == 0) restAtTopSinceMs = now;
    if ((now - restAtTopSinceMs) >= FULL_REST_MS) {
      declareBatteryFull(now);
      restAtTopSinceMs = 0;
    }
  } else {
    restAtTopSinceMs = 0;
  }

#if !BAME_WIRING_BUS
  // LOAD install: invisible partial charges leave the integrator stale. Watch
  // the slow voltage average — any rise > 50 mV that doesn't culminate in a
  // full event flags the SOC as uncertain. Cleared by the next full event.
  if (voltage - vSlowAvg > 0.05f) {
    socUncertain = true;
  }
  vSlowAvg = vSlowAvg * 0.99f + voltage * 0.01f;
#endif

  // Periodic save so a power loss doesn't wipe the integrator
  if ((now - lastEepromSaveMs) >= EEPROM_SAVE_INTERVAL_MS) {
    saveCoulombEEPROM();
    lastEepromSaveMs = now;
  }
}

// ===========================================================================
// Setup / loop
// ===========================================================================
void setup() {
#if BAME_DEBUG
  Serial.begin(115200);
  Serial.println(F("\nBaMe v" BAME_VERSION " (coulomb-only)"));
#endif

  Wire.begin();
  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
  }
  ina.begin();
  ina.setMaxCurrentShunt(MAX_CURRENT, SHUNT_RESISTANCE);
  ina.setAverage(4);

  pinMode(ACTION_BTN_PIN, INPUT_PULLUP);
  loadKeypadEEPROM();
  computeKeypadThresholds();

  // Persistent state. Order matters: nom first (sets default cap), then
  // learned (overrides), then coulomb running total.
  float v;
  if (loadFloatEEPROM(EEPROM_NOM_MAGIC_ADDR, EEPROM_NOM_VAL, EEPROM_NOM_DATA_ADDR, v)
      && v >= CAPACITY_MIN && v <= CAPACITY_MAX) {
    batteryCapacityNom = v;
  }
  setCapacityNom(batteryCapacityNom);
  if (loadFloatEEPROM(EEPROM_LEARN_MAGIC_ADDR, EEPROM_LEARN_VAL, EEPROM_LEARN_DATA_ADDR, v)
      && v >= CAPACITY_MIN && v <= CAPACITY_MAX) {
    batteryCapacityAh = v;
    capacityLearned = true;
  }
  if (!loadFloatEEPROM(EEPROM_COULOMB_MAGIC_ADDR, EEPROM_COULOMB_VAL,
                       EEPROM_COULOMB_DATA_ADDR, coulombCount)) {
    coulombCount = capacityAs();
    socUncertain = true;
  }

  lastMeasure = millis();
  lastEepromSaveMs = millis();
}

void loop() {
  unsigned long now = millis();
  if ((now - lastMeasure) >= MEASURE_INTERVAL_MS) {
    updateMeasurements();
  }
  if ((now - lastDisplay) >= DISPLAY_INTERVAL_MS) {
    updateDisplay();
    lastDisplay = now;
  }

  // Long-press CENTER (≥ 500 ms) opens the settings menu.
  if (readButton() == BTN_CENTER) {
    unsigned long pressStart = millis();
    while (readButton() == BTN_CENTER) {
      if (millis() - pressStart >= 500) {
        waitButtonRelease();
        settingsMenu();
        break;
      }
      delay(20);
    }
  }
}
