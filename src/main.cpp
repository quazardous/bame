// ===========================================================================
// BaMe v2 — pure coulomb counting (plug-and-forget build)
//
// Voltage is a display value plus a trigger for the "battery full" event
// (sustained top OCV at rest → SOC reset to 100%). Capacity is learned
// automatically: BUS installs measure it bidirectionally between the FULL and
// KNEE rest anchors (aging-aware); LOAD installs raise-only from the deepest
// discharge. A BMS cutoff cuts BaMe's own power, so it is never observed.
//
// No physical button and no settings menu: the device is installed once and
// left to run. Capacity starts at the compile-time nominal and refines itself.
// ===========================================================================

#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <Adafruit_SSD1306.h>
#include <INA226.h>
#include "BameGFX.h"
#include "bame_state.h"
#include "bame_core.h"
#include "display.h"

// BAME_VERSION is defined in bame_state.h (shared with display.cpp).

#ifndef BAME_DEBUG
  #define BAME_DEBUG 0
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

// --- INA226 ---
#define SHUNT_RESISTANCE 0.0025f
#define MAX_CURRENT      30.0f

// --- User defaults ---
#define BATTERY_CAPACITY_AH    80.0f
#define CAPACITY_MIN           1.0f
#define CAPACITY_MAX           500.0f

// --- Smoothing / pacing ---
#define MEASURE_INTERVAL_MS    100
#define DISPLAY_INTERVAL_MS    500
#define EEPROM_SAVE_INTERVAL_MS 300000UL  // save coulombCount every 5 min

// --- Screen power management ---
// The OLED (SSD1306, no backlight — pixels self-emit) sleeps via a software
// command after SCREEN_TIMEOUT_MS with no electrical activity. There is no
// wake button in the install, so the wake source is the measurement itself:
// any current past the dead-band (either sign) or any voltage step re-arms
// the timer / wakes the panel. Power-on arms it too → 2 min on after boot.
#define SCREEN_TIMEOUT_MS  120000UL  // sleep after 2 min with no activity
#define SCREEN_WAKE_I      0.05f     // |current| (A) past the dead-band = activity
#define SCREEN_WAKE_DV     0.05f     // voltage step (V) between ticks = activity

// --- EEPROM layout ---
#define EEPROM_LEARN_MAGIC_ADDR  16
#define EEPROM_LEARN_VAL         0xEE
#define EEPROM_LEARN_DATA_ADDR   17      // float
#define EEPROM_COULOMB_MAGIC_ADDR 21
#define EEPROM_COULOMB_VAL       0xCC
#define EEPROM_COULOMB_DATA_ADDR 22      // float

// ===========================================================================
// Hardware objects (definitions for externs in bame_state.h)
// ===========================================================================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
BameGFX gfx(display);
INA226 ina(INA226_ADDR, &Wire);

// --- State definitions ---
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
bool  chargingExternal = false;

bool  batteryPresent = false;

unsigned long lastMeasure = 0;
unsigned long lastDisplay = 0;

static unsigned long lastEepromSaveMs = 0;

// ===========================================================================
// EEPROM persistence (learned capacity + running coulomb counter)
// ===========================================================================
static void saveFloatEEPROM(uint8_t magicAddr, uint8_t magicVal, uint8_t dataAddr, float val) {
  EEPROM.update(magicAddr, magicVal);
  EEPROM.put(dataAddr, val);   // EEPROM.put already skips unchanged bytes
}
static bool loadFloatEEPROM(uint8_t magicAddr, uint8_t magicVal, uint8_t dataAddr, float &val) {
  if (EEPROM.read(magicAddr) != magicVal) return false;
  EEPROM.get(dataAddr, val);
  return true;
}

static void saveLearnedEEPROM() {
  saveFloatEEPROM(EEPROM_LEARN_MAGIC_ADDR, EEPROM_LEARN_VAL, EEPROM_LEARN_DATA_ADDR, batteryCapacityAh);
}
static void saveCoulombEEPROM() {
  saveFloatEEPROM(EEPROM_COULOMB_MAGIC_ADDR, EEPROM_COULOMB_VAL, EEPROM_COULOMB_DATA_ADDR, coulombCount);
}

// ===========================================================================
// BaMe core wrapper — the algo runs out of bame_core.c (shared with sim).
// This layer handles Arduino-specific plumbing: INA226 reads, EEPROM
// persistence, and mirroring the state into the display externs.
// ===========================================================================
static bame_state_t  bame;
static bame_config_t bame_cfg;

// ===========================================================================
// Screen power management (no wake button in the install → activity-driven)
// ===========================================================================
static bool          screenOn       = true;
static unsigned long lastActivityMs = 0;
static float         prevVoltage    = 0;

// Wake the panel (if asleep) and extend the on-time by SCREEN_TIMEOUT_MS.
static void screenWake() {
  lastActivityMs = millis();
  if (!screenOn) {
    display.ssd1306_command(SSD1306_DISPLAYON);
    screenOn = true;
    updateDisplay();            // repaint now, don't wait for the 500 ms tick
    lastDisplay = millis();
  }
}

static void screenSleep() {
  if (screenOn) {
    display.ssd1306_command(SSD1306_DISPLAYOFF);  // ~10 µA, panel off
    screenOn = false;
  }
}

// Called once per measurement tick: decide wake vs sleep from the fresh
// voltage/current globals. `now` is the measurement's millis timestamp.
static void updateScreenPower(unsigned long now) {
  bool active = (fabsf(current) > SCREEN_WAKE_I)
             || (fabsf(voltage - prevVoltage) > SCREEN_WAKE_DV);
  prevVoltage = voltage;
  if (active) {
    screenWake();
  } else if (screenOn && (now - lastActivityMs) >= SCREEN_TIMEOUT_MS) {
    screenSleep();
  }
}

static void updateMeasurements() {
  float v_raw = ina.getBusVoltage();
  float i_raw = ina.getCurrent();
  power = ina.getPower();

  unsigned long now = millis();
  float dt = (now - lastMeasure) / 1000.0f;
  if (dt <= 0 || dt > 1.0f) dt = 0;
  lastMeasure = now;

  bame_event_t evt = bame_step(&bame, &bame_cfg, v_raw, i_raw, dt, (uint32_t)now);

  // Mirror core state to the externs used by display.cpp.
  voltage         = bame.voltage;
  current         = bame.current;
  currentOffset   = bame.current_offset;
  cAvg            = bame.c_avg;
  cAvgInit        = bame.c_avg_init;
  coulombCount    = bame.coulomb_count;
  socUncertain    = bame.soc_uncertain;
  chargingExternal = bame.charging_external;
  batteryPresent  = bame.battery_present;
  capacityLearned = bame.capacity_learned;
  batteryCapacityAh = bame.capacity_ah;

  // Persist on FULL — capacity may have changed (learning) and coulomb_count
  // was reset to top.
  if (evt == BAME_EVT_FULL) {
    saveCoulombEEPROM();
    saveLearnedEEPROM();
  }

  // Periodic save so a power loss doesn't wipe the integrator.
  if ((now - lastEepromSaveMs) >= EEPROM_SAVE_INTERVAL_MS) {
    saveCoulombEEPROM();
    lastEepromSaveMs = now;
  }

  updateScreenPower(now);
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

  // Initialize bame core with build-flag config.
  bame_config_defaults(&bame_cfg);
  bame_init(&bame, BAME_CELLS, (BAME_WIRING_BUS != 0), BATTERY_CAPACITY_AH);

  // Restore persistent state: learned capacity (measured) + running coulomb
  // counter. Each slot is independent — a missing slot leaves the core at its
  // init defaults (compile-time nominal capacity, assumed-full counter).
  float v;
  if (loadFloatEEPROM(EEPROM_LEARN_MAGIC_ADDR, EEPROM_LEARN_VAL, EEPROM_LEARN_DATA_ADDR, v)
      && v >= CAPACITY_MIN && v <= CAPACITY_MAX) {
    bame.capacity_ah      = v;
    bame.capacity_learned = true;
    batteryCapacityAh     = v;
    capacityLearned       = true;
  }
  if (loadFloatEEPROM(EEPROM_COULOMB_MAGIC_ADDR, EEPROM_COULOMB_VAL,
                      EEPROM_COULOMB_DATA_ADDR, v)) {
    bame.coulomb_count = v;
    coulombCount       = v;
  }

  lastMeasure = millis();
  lastEepromSaveMs = millis();
  lastActivityMs = millis();   // power-on counts as activity → screen on for 2 min
}

void loop() {
  unsigned long now = millis();
  if ((now - lastMeasure) >= MEASURE_INTERVAL_MS) {
    updateMeasurements();
  }
  if ((now - lastDisplay) >= DISPLAY_INTERVAL_MS) {
    if (screenOn) updateDisplay();   // skip the I2C repaint while the panel sleeps
    lastDisplay = now;
  }
}
