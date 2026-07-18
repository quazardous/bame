// Shared state for BaMe v2 (pure coulomb counting, no voltage SOC).
// Globals are defined in main.cpp; modules read them via these externs.
//
// Plug-and-forget build: no physical button, no settings menu. Capacity is
// the compile-time nominal until a cycle learns the real value; everything
// else is automatic.
#pragma once
#include <Arduino.h>
#include <Adafruit_SSD1306.h>
#include <INA226.h>
#include "BameGFX.h"

// --- Firmware version (serial banner + no-battery splash) ---
#define BAME_VERSION "2.16"

// --- Hardware objects ---
extern Adafruit_SSD1306 display;
extern BameGFX gfx;
extern INA226 ina;

// --- Battery capacity (learned, or compile-time nominal until learned) ---
extern float batteryCapacityAh;
extern bool  capacityLearned;      // true once a cycle has measured the capacity

// --- Live measurements ---
extern float voltage;
extern float current;              // INA reading minus offset, dead-band applied
extern float power;                // INA-reported power
extern float currentOffset;
extern float cAvg;                 // EWMA-smoothed current, τ ≈ 30 s (watts)
extern float cAvgSlow;             // EWMA-smoothed current, τ ≈ 1 h  (autonomy)
extern bool  cAvgInit;

// --- SOC integrator (single source of truth) ---
extern float coulombCount;
extern bool  socUncertain;
extern bool  chargingExternal;     // LOAD-mode: charger detected, integration frozen
extern float deliveredAh;          // peak Ah delivered since last full (provisional hint)

// --- Battery presence ---
extern bool  batteryPresent;

// --- Loop pacing ---
extern unsigned long lastMeasure;
extern unsigned long lastDisplay;

// --- Capacity / SOC helpers ---
inline float capacityAs() { return batteryCapacityAh * 3600.0f; }
inline float socPercent() {
  float as = capacityAs();
  if (as <= 0) return 0;
  return constrain(coulombCount / as * 100.0f, 0.0f, 100.0f);
}
