// Shared state for BaMe v2 (pure coulomb counting, no voltage SOC).
// Globals are defined in main.cpp; modules read them via these externs.
#pragma once
#include <Arduino.h>
#include <Adafruit_SSD1306.h>
#include <INA226.h>
#include "BameGFX.h"

// --- Hardware objects ---
extern Adafruit_SSD1306 display;
extern BameGFX gfx;
extern INA226 ina;

// --- Cell count (compile-time, BAME_CELLS in platformio.ini) ---
extern const uint8_t cellCount;

// --- Battery capacity ---
extern float batteryCapacityNom;   // user "sticker value", reset target
extern float batteryCapacityAh;    // learned (or nom if not yet learned)
extern bool  capacityLearned;      // true after at least one full→cutoff cycle

// --- Live measurements ---
extern float voltage;
extern float current;              // INA reading minus offset, dead-band applied
extern float power;                // INA-reported power
extern float currentOffset;
extern float cAvg;                 // EWMA-smoothed current (display only)
extern bool  cAvgInit;

// --- SOC integrator (single source of truth) ---
extern float coulombCount;
extern bool  socUncertain;
extern bool  chargingExternal;  // LOAD-mode: charger detected, integration frozen         // LOAD mode: true after invisible partial charge

// --- Battery presence + cycle bookkeeping ---
extern bool  batteryPresent;
extern float coulombsAtLastFull;   // coulombCount value at the last full event
extern unsigned long sinceLastFullMs;

// --- Loop pacing ---
extern unsigned long lastMeasure;
extern unsigned long lastDisplay;

// --- Buttons ---
enum Button { BTN_NONE, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_CENTER };
Button readButton();
Button readButtonDebounced();
void   waitButtonRelease();

// --- EEPROM helpers (defined in main.cpp) ---
void saveNomEEPROM();
void saveLearnedEEPROM();
void saveCoulombEEPROM();
void resetAllEEPROM();

// --- Capacity / event helpers (defined in main.cpp) ---
inline float capacityAs() { return batteryCapacityAh * 3600.0f; }
inline float socPercent() {
  float as = capacityAs();
  if (as <= 0) return 0;
  return constrain(coulombCount / as * 100.0f, 0.0f, 100.0f);
}
void setCapacityNom(float ah);
void declareBatteryFull(unsigned long now);
