// Shared global state — declarations only. Definitions live in main.cpp so
// the v1 split stays mechanical (no behavior change vs single-file build).
#pragma once
#include <Arduino.h>
#include <Adafruit_SSD1306.h>
#include <INA226.h>
#include "BameGFX.h"

// Hardware objects
extern Adafruit_SSD1306 display;
extern BameGFX gfx;
extern INA226 ina;

// Battery / capacity
extern const uint8_t cellCount;
extern float vbatTop;
extern float vbatBottom;
extern float vMinUtile;
extern float vMaxUtile;
extern float batteryCapacityNom;
extern float batteryCapacityAh;
extern float batteryCapacityAs;

// Live measurements
extern float voltage;
extern float current;
extern float power;
extern float socPercent;
extern float coulombCount;
extern float coulombRaw;
extern float currentOffset;
extern float cAvg;
extern float maxSliceI;
extern float bufMin;

// Trend / charge / chrono
extern bool externalChargeDetected;
extern int8_t voltageTrend;
#if BAME_DEV
extern unsigned long flatSince;
#endif

// Calibration segment state
extern float calCoulombs;
extern float calChargeSec;
extern float calTarget;
extern float calStartVoltage;
extern unsigned long calStartMs;
#if BAME_DEV
extern float pendingEndVoltage;
extern float pendingEndCoulombs;
extern unsigned long pendingEndMs;
#endif

// Sleep / interaction
#if !BAME_DEV
extern bool autoDeepSleep;
extern bool oledSleeping;
extern unsigned long lastInteraction;
#else
#define autoDeepSleep false
#endif

// Loop pacing
extern unsigned long lastMeasure;
extern unsigned long lastDisplay;

// Buttons
enum Button { BTN_NONE, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_CENTER };
Button readButton();
Button readButtonDebounced();
void waitButtonRelease();

// EEPROM helpers (defined in main.cpp)
void saveNomToEEPROM();
void saveVcalToEEPROM();
void saveCapToEEPROM();
void resetCapEEPROM();
#if !BAME_DEV
void saveAutoSleepToEEPROM();
#endif

// Capacity setter
void setCapacity(float ah);
void resetCalibration();
float socFromVoltage(float v);
