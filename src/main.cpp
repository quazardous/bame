#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <avr/sleep.h>
#include <avr/power.h>
#include <avr/wdt.h>
#include <Adafruit_SSD1306.h>
#include <INA226.h>
#include "BameGFX.h"

// --- Configuration ---
#define BAME_VERSION "1.3"

#ifndef BAME_DEBUG
  #define BAME_DEBUG 0
#endif

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C

// Bicolor screen zones defined in BameGFX.h
#define INA226_ADDR 0x40

// Foxeer Key23 keypad on analog pin
#define KEY_PIN A3
// Action button (NO, pull to GND)
#define ACTION_BTN_PIN 2

// LiFePO4 4S 80Ah battery
#define BATTERY_CAPACITY_AH 80.0
#define SHUNT_RESISTANCE 0.0025
#define MAX_CURRENT 30.0

// LFP 4S voltage thresholds (factory defaults, overridden by EEPROM)
#define VBAT_FULL_DEFAULT   14.6  // 4 x 3.65V
#define VBAT_EMPTY_DEFAULT  10.0  // 4 x 2.50V

// Detection thresholds
#define VBAT_REST_CURRENT  0.3    // max current to consider at rest
#define VBAT_CONVERGE_FAST 0.01   // fast convergence (first calibration)
#define VBAT_CONVERGE_SLOW 0.001  // slow convergence (continuous adjustment)

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

// addr 31-35 : free

// EEPROM layout for auto deep sleep
#define EEPROM_ASLEEP_ADDR        36  // 1 byte: 0x01 = active

// Dynamic voltages
float vbatMax = VBAT_FULL_DEFAULT;
float vbatMin = VBAT_EMPTY_DEFAULT;
float lastRestVoltage = 0;

// Battery capacity
float batteryCapacityNom = BATTERY_CAPACITY_AH;  // nominal (user)
float batteryCapacityAh = BATTERY_CAPACITY_AH;   // estimated (calibration or nominal fallback)
float batteryCapacityAs = BATTERY_CAPACITY_AH * 3600.0;
bool capacityKnown = false;   // true if reliable calibration

// Capacity calibration by exponential doubling
float calCoulombs = 0;          // accumulated coulombs for current segment
float calTarget = 0;            // coulomb target (0 = initial time mode)
float calStartVoltage = 0;      // rest voltage at segment start
unsigned long calStartMs = 0;   // segment start timestamp
#define CAL_INITIAL_TIME_MS  60000   // 1 min for first step
#define CAL_MIN_COULOMBS     500.0   // ~0.14Ah minimum (INA226 precision)

bool autoDeepSleep = false;   // auto deep sleep after inactivity timeout

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
void updateVoltageCalibration();
float socFromVoltage(float v);
void settingsMenu();
void batteryMenu();

// Sleep
#define MENU_PRESS_MS      500  // long press -> system menu
#define DEEPSLEEP_PRESS_MS 3000 // longer press -> deep sleep
#define AUTO_SLEEP_MS    60000   // 60s no interaction -> screen sleep
#define AUTO_DEEPSLEEP_MS 300000 // 5min no interaction -> deep sleep
bool oledSleeping = false;
unsigned long lastInteraction = 0;

// ISR for deep sleep wake-up (INT0 on D2)
volatile bool wakeUpFlag = false;
void wakeUpISR() {
  wakeUpFlag = true;
}

void enterOledSleep() {
  oledSleeping = true;
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  #if BAME_DEBUG
  Serial.println(F("[SLEEP] OLED off"));
  #endif
}

void exitOledSleep() {
  oledSleeping = false;
  display.ssd1306_command(SSD1306_DISPLAYON);
  #if BAME_DEBUG
  Serial.println(F("[SLEEP] OLED on"));
  #endif
}

// Watchdog ISR for periodic wake-up
volatile bool wdtWakeUp = false;
ISR(WDT_vect) {
  wdtWakeUp = true;
}

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

        // Measure
        Wire.begin();
        if (ina.begin()) {
          ina.setMaxCurrentShunt(MAX_CURRENT, SHUNT_RESISTANCE);
          voltage = ina.getBusVoltage();
          current = ina.getCurrent();
          updateVoltageCalibration();

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

// EEPROM layout for keypad calibration
#define EEPROM_MAGIC_ADDR 0        // 1 byte: 0xCA = calibrated
#define EEPROM_CAL_ADDR   1        // 5 x 2 bytes (int) = 10 bytes
#define EEPROM_MAGIC_VAL  0xCA
#define CAL_BTN_COUNT 5

// Order: CENTER, UP, DOWN, LEFT, RIGHT (sorted: 1, 416, 616, 748, 838)
// Gaps: 1-416=415, 416-616=200, 616-748=132, 748-838=90, 838-1023=185
// Tolerance = min(left gap, right gap) / 2
int keyCalVals[CAL_BTN_COUNT] = {838, 616, 1, 748, 416}; // defaults with 10k pullup

bool keyCalibrated = false;

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

// --- Voltage calibration EEPROM ---
void saveVcalToEEPROM() {
  EEPROM.write(EEPROM_VCAL_MAGIC_ADDR, EEPROM_VCAL_MAGIC_VAL);
  EEPROM.put(EEPROM_VCAL_ADDR, vbatMax);
  EEPROM.put(EEPROM_VCAL_ADDR + sizeof(float), vbatMin);
}

bool loadVcalFromEEPROM() {
  if (EEPROM.read(EEPROM_VCAL_MAGIC_ADDR) != EEPROM_VCAL_MAGIC_VAL) return false;
  EEPROM.get(EEPROM_VCAL_ADDR, vbatMax);
  EEPROM.get(EEPROM_VCAL_ADDR + sizeof(float), vbatMin);
  // Sanity check
  if (vbatMax < 10.0 || vbatMax > 20.0) vbatMax = VBAT_FULL_DEFAULT;
  if (vbatMin < 5.0 || vbatMin > 15.0) vbatMin = VBAT_EMPTY_DEFAULT;
  if (vbatMin >= vbatMax) { vbatMax = VBAT_FULL_DEFAULT; vbatMin = VBAT_EMPTY_DEFAULT; }
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
  if (batteryCapacityAh < 1.0 || batteryCapacityAh > 500.0) batteryCapacityAh = batteryCapacityNom;
  batteryCapacityAs = batteryCapacityAh * 3600.0;
  capacityKnown = true;
  return true;
}

// Nominal capacity (user)
#define saveNomToEEPROM() saveFloatEEPROM(EEPROM_NOM_MAGIC_ADDR, EEPROM_NOM_MAGIC_VAL, EEPROM_NOM_ADDR, batteryCapacityNom)
bool loadNomFromEEPROM() {
  if (!loadFloatEEPROM(EEPROM_NOM_MAGIC_ADDR, EEPROM_NOM_MAGIC_VAL, EEPROM_NOM_ADDR, batteryCapacityNom)) return false;
  if (batteryCapacityNom < 1.0 || batteryCapacityNom > 500.0) batteryCapacityNom = BATTERY_CAPACITY_AH;
  return true;
}

void resetCapEEPROM() {
  EEPROM.write(EEPROM_CAP_MAGIC_ADDR, 0xFF);
  capacityKnown = false;
  batteryCapacityAh = batteryCapacityNom;  // fallback to nominal
  batteryCapacityAs = batteryCapacityNom * 3600.0;
  calCoulombs = 0;
  calTarget = 0;
  calStartVoltage = 0;
  calStartMs = 0;
}

void saveAutoSleepToEEPROM() { EEPROM.write(EEPROM_ASLEEP_ADDR, autoDeepSleep ? 0x01 : 0x00); }
void loadAutoSleepFromEEPROM() { autoDeepSleep = (EEPROM.read(EEPROM_ASLEEP_ADDR) == 0x01); }

// Voltage auto-calibration update
// Conditions: at rest (|current| < threshold) AND not charging (current >= 0)
unsigned long lastVcalSave = 0;
#define VCAL_SAVE_INTERVAL 60000  // save to EEPROM at most every 60s

void updateVoltageCalibration() {
  // Ignore if charging (artificially high voltage)
  if (current < -VBAT_REST_CURRENT) return;
  // Ignore if not at rest
  if (abs(current) > VBAT_REST_CURRENT) return;

  lastRestVoltage = voltage;

  // Convergence speed: fast if far, slow if close
  float conv;

  // Update Vmax
  if (voltage > vbatMax * 0.98) {
    conv = (voltage > vbatMax) ? VBAT_CONVERGE_FAST : VBAT_CONVERGE_SLOW;
    vbatMax = vbatMax * (1.0 - conv) + voltage * conv;
  }

  // Update Vmin
  if (voltage < vbatMin * 1.05) {
    conv = (voltage < vbatMin) ? VBAT_CONVERGE_FAST : VBAT_CONVERGE_SLOW;
    vbatMin = vbatMin * (1.0 - conv) + voltage * conv;
  }

  // Periodically save to EEPROM (not every measurement)
  if (millis() - lastVcalSave >= VCAL_SAVE_INTERVAL) {
    saveVcalToEEPROM();
    lastVcalSave = millis();
  }
}

// Thresholds computed dynamically from calibrated values
// Each button has its own tolerance = distance to nearest neighbor / 2
int keyThresholds[CAL_BTN_COUNT];

void computeThresholds() {
  // Sort values to find distances
  int sorted[CAL_BTN_COUNT + 1]; // +1 pour NONE (1023)
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
  bool menuTriggered = false;

  while (readButton() == BTN_CENTER) {
    unsigned long held = millis() - pressStart;

    if (held >= MENU_PRESS_MS) {
      // Tier 2 gauge: filling toward deep sleep
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

    delay(50);
  }

  // Released — check which tier was reached
  unsigned long held = millis() - pressStart;
  if (held >= MENU_PRESS_MS) {
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

const SocPoint socCurve[] = {
  {14.6, 100}, {13.6,  99}, {13.4,  90}, {13.3,  70},
  {13.2,  40}, {13.1,  30}, {13.0,  20}, {12.8,  17},
  {12.0,  14}, {11.0,   9}, {10.0,   0},
};
const int socCurveSize = sizeof(socCurve) / sizeof(socCurve[0]);

float socFromVoltage(float v) {
  // Rescale voltage to dynamic bounds
  // Curve is defined for VBAT_EMPTY_DEFAULT..VBAT_FULL_DEFAULT
  // Map v into that range using vbatMin..vbatMax
  float vScaled = VBAT_EMPTY_DEFAULT +
    (v - vbatMin) / (vbatMax - vbatMin) *
    (VBAT_FULL_DEFAULT - VBAT_EMPTY_DEFAULT);

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

// --- Hardware diag (all in one screen) ---
extern int __heap_start, *__brkval;
int freeRAM() {
  int v;
  return (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
}

void diagHW(const __FlashStringHelper* title) {
  while (true) {
    display.clearDisplay();
    gfx.drawTitle(title);
    display.setTextSize(1);
    display.setCursor(0, BLUE_Y);

    // Line 1: Version + RAM
    display.print(F("v" BAME_VERSION " RAM:"));
    display.print(freeRAM());

    // Line 2: I2C devices
    display.setCursor(0, BLUE_Y + 9);
    display.print(F("I2C:"));
    for (byte addr = 1; addr < 127; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0) {
        display.print(F(" 0x"));
        if (addr < 16) display.print('0');
        display.print(addr, HEX);
      }
    }

    // Line 3: INA226 live
    display.setCursor(0, BLUE_Y + 18);
    float v = ina.getBusVoltage();
    float a = ina.getCurrent();
    display.print(v, 2); display.print(F("V "));
    display.print(a, 2); display.print(F("A "));
    display.print(v * abs(a), 1); display.print('W');

    // Line 4: Uptime
    display.setCursor(0, BLUE_Y + 27);
    display.print(F("Up:"));
    unsigned long s = millis() / 1000;
    if (s >= 3600) { display.print(s / 3600); display.print(F("h")); }
    if (s >= 60) { display.print((s % 3600) / 60); display.print(F("m")); }
    display.print(s % 60); display.print('s');

    display.display();

    Button b = readButtonDebounced();
    if (b == BTN_LEFT) break;
    delay(200);
  }
  waitButtonRelease();
}

// --- Battery: info page (read only) ---
void batteryInfo(const __FlashStringHelper* title) {
  while (true) {
    display.clearDisplay();
    gfx.drawTitle(title);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    // Line 1: Estimated capacity [nominal]
    display.setCursor(0, BLUE_Y);
    display.print(F("Cap:"));
    if (capacityKnown) {
      display.print((int)batteryCapacityAh);
    } else {
      display.print(F("--"));
    }
    display.print(F("Ah ["));
    display.print((int)batteryCapacityNom);
    display.print(F("]"));

    // Line 2: Calibration + delta%
    display.setCursor(0, BLUE_Y + 9);
    float calAh = calCoulombs / 3600.0;
    display.print(calAh, calAh < 1.0 ? 3 : 1);
    display.print(F("/"));
    if (calTarget > 0) { float tAh = calTarget / 3600.0; display.print(tAh, tAh < 1.0 ? 3 : 0); }
    else display.print('-');
    display.print(F("Ah"));
    if (calStartVoltage > 0 && lastRestVoltage > 0) {
      int ds = (int)(socFromVoltage(calStartVoltage) - socFromVoltage(lastRestVoltage));
      if (ds > 0) { display.print(F(" (")); display.print(ds); display.print(F("%)")); }
    }

    // Line 3: Segment V
    display.setCursor(0, BLUE_Y + 18);
    if (calStartVoltage > 0) display.print(calStartVoltage, 2);
    else display.print('-');
    display.print(F(">"));
    if (lastRestVoltage > 0) display.print(lastRestVoltage, 2);
    else display.print('-');
    display.print('V');

    // Line 4: Vmax/Vmin
    display.setCursor(0, BLUE_Y + 27);
    display.print(vbatMax, 2);
    display.print('/');
    display.print(vbatMin, 2);
    display.print('V');

    display.display();

    Button b = readButtonDebounced();
    if (b == BTN_LEFT || b == BTN_CENTER) break;
    delay(100);
  }
  waitButtonRelease();
}

// --- SYSTEM menu (flat) ---
#define SYS_COUNT 6

void settingsMenu() {
  uint8_t sel = 0;
  int8_t editing = -1;

  float tmpCap = batteryCapacityNom;
  bool tmpConfirm = false;  // shared for all YES/NO confirmations
  bool tmpSleep = autoDeepSleep;

  while (true) {
    display.clearDisplay();
    gfx.drawTitle(F("Bame v" BAME_VERSION));

    char buf[10];

    // 0: Capacity
    int capVal = (int)(editing == 0 ? tmpCap : batteryCapacityNom);
    itoa(capVal, buf, 10);
    strcat(buf, "Ah");
    gfx.drawMenuItem(0, ' ', F("Capacity"), buf, sel == 0, editing == 0);

    // 1: Sleep
    gfx.drawMenuItem(1, ' ', F("Eco mode"),
      (editing == 1 ? tmpSleep : autoDeepSleep) ? "ON" : "OFF",
      sel == 1, editing == 1);

    // 2: Info cal
    gfx.drawMenuItem(2, '>', F("Info cal"), NULL, sel == 2);

    // 3: Reset cal
    gfx.drawMenuItem(3, ' ', F("Reset cal"),
      editing == 3 ? (tmpConfirm ? "YES" : "NO") : "",
      sel == 3, editing == 3);

    // 4: Hardware
    gfx.drawMenuItem(4, '>', F("Hardware"), NULL, sel == 4);

    // 5: Reset ALL
    gfx.drawMenuItem(5, ' ', F("Reset ALL"),
      editing == 5 ? (tmpConfirm ? "YES" : "NO") : "",
      sel == 5, editing == 5);

    display.display();

    Button b = readButtonDebounced();
    if (b == BTN_NONE) { delay(50); continue; }

    if (editing >= 0) {
      switch (b) {
        case BTN_UP:
          if (editing == 0) { tmpCap += 1; if (tmpCap > 500) tmpCap = 500; }
          else if (editing == 1) { tmpSleep = !tmpSleep; }
          else if (editing == 3 || editing == 5) { tmpConfirm = !tmpConfirm; }
          break;
        case BTN_DOWN:
          if (editing == 0) { tmpCap -= 1; if (tmpCap < 1) tmpCap = 1; }
          else if (editing == 1) { tmpSleep = !tmpSleep; }
          else if (editing == 3 || editing == 5) { tmpConfirm = !tmpConfirm; }
          break;
        case BTN_CENTER:
          if (editing == 0) {
            batteryCapacityNom = tmpCap;
            saveNomToEEPROM();
            if (!capacityKnown) {
              batteryCapacityAh = batteryCapacityNom;
              batteryCapacityAs = batteryCapacityNom * 3600.0;
            }
          } else if (editing == 1) {
            autoDeepSleep = tmpSleep;
            saveAutoSleepToEEPROM();
          } else if (editing == 3) {
            if (tmpConfirm) resetCapEEPROM();
          } else if (editing == 5) {
            if (tmpConfirm) {
              for (uint16_t i = 0; i < E2END + 1; i++) EEPROM.write(i, 0xFF);
              // Software reboot via watchdog
              wdt_enable(WDTO_15MS);
              while (true);
            }
          }
          editing = -1;
          break;
        case BTN_LEFT:
          tmpCap = batteryCapacityNom;
          tmpConfirm = false;
          tmpSleep = autoDeepSleep;
          editing = -1;
          break;
        default: break;
      }
    } else {
      switch (b) {
        case BTN_UP: sel = (sel == 0) ? SYS_COUNT - 1 : sel - 1; break;
        case BTN_DOWN: sel = (sel + 1) % SYS_COUNT; break;
        case BTN_CENTER:
          if (sel == 2) { waitButtonRelease(); batteryInfo(F("Info cal")); }
          else if (sel == 4) { waitButtonRelease(); diagHW(F("Hardware")); }
          else {
            editing = sel;
            tmpCap = batteryCapacityNom;
            tmpConfirm = false;
            tmpSleep = autoDeepSleep;
          }
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
float readCurrent() { return ina.getCurrent(); }
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
  if (voltage < 1.0) {
    batteryPresent = false;
    socPercent = 0;
    coulombCount = 0;
    return;
  }

  // 1) Init: wait for stable voltage before locking SOC
  if (!batteryPresent) {
    socPercent = socFromVoltage(voltage);
    coulombCount = (socPercent / 100.0) * batteryCapacityAs;
    if (voltage >= vbatMin) {
      batteryPresent = true;
    }
    return;
  }

  // 2) Coulomb counting
  coulombCount -= current * dtSeconds;
  if (coulombCount > batteryCapacityAs) coulombCount = batteryCapacityAs;
  if (coulombCount < 0) coulombCount = 0;
  socPercent = (coulombCount / batteryCapacityAs) * 100.0;

  // 3) Exponential doubling calibration
  //    Accumulate discharge coulombs. A partial recharge invalidates
  //    the segment because the voltage-SOC mapping becomes unreliable
  //    after a charge event (surface charge, hysteresis).
  if (current > 0) {
    calCoulombs += current * dtSeconds;
  } else if (current < -VBAT_REST_CURRENT) {
    // Charging detected: invalidate current segment
    calCoulombs = 0;
    calTarget = 0;
    calStartVoltage = 0;
  }

  // 4) Rest detection (stable for 5s before trusting voltage)
  //    LFP cells need a few seconds after load removal for the voltage
  //    to settle (internal resistance + diffusion). Readings taken
  //    immediately after load removal can be 20-50mV off.
  static unsigned long restSince = 0;
  if (abs(current) < VBAT_REST_CURRENT) {
    if (restSince == 0) restSince = now;
    bool stableRest = (now - restSince >= 5000);

    // SOC blend: only after 5s stable rest, gentle correction (5%)
    //   Without this guard the blend runs every 100ms and converges
    //   to the voltage estimate in ~3s, making coulomb counting
    //   pointless at rest. On the flat LFP curve (13.1-13.4V spans
    //   30-90% SOC), a 10mV error → ~5% SOC jump if blended too fast.
    if (stableRest) {
      float socV = socFromVoltage(voltage);
      socPercent = socPercent * 0.95 + socV * 0.05;
      coulombCount = (socPercent / 100.0) * batteryCapacityAs;
      lastRestVoltage = voltage;
      // Initialize segment start if not set yet
      if (calStartVoltage == 0) {
        calStartVoltage = voltage;
        calStartMs = now;
      }
    }
  } else {
    restSince = 0;
  }

  // 5) Calibration save: target reached AND 5s stable rest AND valid segment
  //    Both start and end voltages must be taken under the same 5s rest
  //    condition to ensure symmetric accuracy.
  bool calTargetReached;
  if (calTarget <= 0) {
    // Initial time mode: 1 min AND minimum coulombs
    calTargetReached = (now - calStartMs >= CAL_INITIAL_TIME_MS)
                    && (calCoulombs >= CAL_MIN_COULOMBS);
  } else {
    // Doubling mode: coulomb target
    calTargetReached = (calCoulombs >= calTarget);
  }

  if (calTargetReached && calStartVoltage > 0 && calCoulombs > 0
      && abs(current) < VBAT_REST_CURRENT && restSince > 0
      && (now - restSince >= 5000)) {
    float vEnd = voltage;
    float socStart = socFromVoltage(calStartVoltage);
    float socEnd = socFromVoltage(vEnd);
    float deltaSoc = socStart - socEnd;

    // Estimate capacity if delta SOC is meaningful
    if (deltaSoc > 5.0) {
      float estAh = (calCoulombs / 3600.0) / (deltaSoc / 100.0);
      if (estAh > 1.0 && estAh < 500.0) {
        if (!capacityKnown) {
          // First estimate: accept only if delta > 30% (reliable)
          if (deltaSoc > 30.0) {
            batteryCapacityAh = estAh;
            batteryCapacityAs = estAh * 3600.0;
            capacityKnown = true;
            saveCapToEEPROM();
            if (!autoDeepSleep) {
              autoDeepSleep = true;
              saveAutoSleepToEEPROM();
            }
          }
        } else {
          // Weighted convergence: larger delta = more weight
          float weight = constrain(deltaSoc / 100.0, 0.05, 0.5);
          batteryCapacityAh = batteryCapacityAh * (1.0 - weight) + estAh * weight;
          batteryCapacityAs = batteryCapacityAh * 3600.0;
          saveCapToEEPROM();
        }
      }
    }

    // Double the target for the next step
    calTarget = calCoulombs * 2.0;
    // New segment starts from current rest voltage
    calStartVoltage = vEnd;
    calStartMs = now;
    calCoulombs = 0;
  }

  updateVoltageCalibration();
}

void updateDisplay() {
  display.clearDisplay();

  // No battery detected
  if (voltage < 1.0) {
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

  // Line 1: Voltage + state
  display.setTextSize(2);
  display.setCursor(0, BLUE_Y + 2);
  display.print(voltage, 1);
  display.setTextSize(1);
  display.print(F("V"));

  // Remaining Ah, right-aligned
  int ahInt = (int)(coulombCount / 3600.0);
  uint8_t ahDigits = 1;
  if (ahInt >= 10)   ahDigits = 2;
  if (ahInt >= 100)  ahDigits = 3;
  int16_t ahX = SCREEN_W - (ahDigits * 12 + 12);
  display.setTextSize(2);
  display.setCursor(ahX, BLUE_Y + 2);
  display.print(ahInt);
  display.setTextSize(1);
  display.print(F("Ah"));

  // Line 2: Power + current
  display.setTextSize(1);
  display.setCursor(0, BLUE_Y + 22);
  display.print((int)abs(power));
  display.print(F("W"));
  // Amps right-aligned
  {
    // Count chars for current with 1 decimal + "A"
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
  int16_t ty = BLUE_Y + 37;
  float hoursLeft = 0;
  if (current > 0.5) {
    hoursLeft = (coulombCount / 3600.0) / current;
  } else if (current < -0.5) {
    float remaining = (batteryCapacityAs - coulombCount) / 3600.0;
    if (remaining < 0) remaining = 0;
    hoursLeft = remaining / (-current);
  }

  if (abs(current) > 0.5) {
    if (hoursLeft < 0) hoursLeft = 0;
    if (hoursLeft > 99.9) hoursLeft = 99.9;
    int h = (int)hoursLeft;
    int m = (int)((hoursLeft - h) * 60);

    // Triangle: < discharge, > charge
    if (current > 0) display.fillTriangle(0, ty + 3, 6, ty, 6, ty + 6, SSD1306_WHITE);
    else             display.fillTriangle(6, ty + 3, 0, ty, 0, ty + 6, SSD1306_WHITE);

    display.setCursor(10, ty);
    char tbuf[6];
    tbuf[0] = '0' + (h / 10); tbuf[1] = '0' + (h % 10);
    tbuf[2] = ':';
    tbuf[3] = '0' + (m / 10); tbuf[4] = '0' + (m % 10);
    tbuf[5] = 0;
    display.print(tbuf);

    if (current < 0) {
      gfx.drawChargingBattery(106, ty);
    }
  }

  // Bottom right: calibration counter (always visible unless charging or eco mode)
  if (current > -VBAT_REST_CURRENT && !autoDeepSleep) {
    bool needsRest = (calStartVoltage == 0)
      || ((calTarget > 0) ? (calCoulombs >= calTarget)
      : ((millis() - calStartMs >= CAL_INITIAL_TIME_MS) && (calCoulombs >= CAL_MIN_COULOMBS)));
    bool blink = (millis() / DISPLAY_INTERVAL_MS) % 2;
    float calAh = calCoulombs / 3600.0;
    // Ah text: blink if blocked, steady otherwise
    if (!needsRest || blink) {
      display.setCursor(SCREEN_W - 30, ty);
      if (calAh >= 10.0) display.print((int)calAh);
      else display.print(calAh, 1);
      display.print(F("Ah"));
    }
    // Play triangle: blink only when accumulating and segment valid
    if (!needsRest && current > 0.5 && blink) {
      display.fillTriangle(SCREEN_W - 38, ty, SCREEN_W - 38, ty + 6, SCREEN_W - 33, ty + 3, SSD1306_WHITE);
    }
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
  if (current > 0.5) {
    float hoursLeft = (coulombCount / 3600.0) / current;
    Serial.print(F(" | Reste: "));
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
  computeThresholds(); // seuils par defaut

  #if BAME_DEBUG
  Serial.println(F(""));
  Serial.println(F("========================="));
  Serial.println(F(" BAME Battery Monitor"));
  Serial.println(F(" LFP 4S 80Ah"));
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
  keyCalibrated = true;

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
      // Appui court -> menu reglages
      settingsMenu();
    }
  }

  // Load voltage calibration from EEPROM
  if (loadVcalFromEEPROM()) {
    #if BAME_DEBUG
    Serial.println(F("[VCAL] EEPROM loaded"));
    Serial.print(F("  Vmax=")); Serial.print(vbatMax, 2);
    Serial.print(F("  Vmin=")); Serial.println(vbatMin, 2);
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
    batteryCapacityAh = batteryCapacityNom;
    batteryCapacityAs = batteryCapacityNom * 3600.0;
  }
  loadAutoSleepFromEEPROM();
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
    ina.setAverage(4);
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
    if (oledSleeping) {
      waitButtonRelease();
      exitOledSleep();
    } else if (b == BTN_CENTER) {
      checkLongPress();
    }
    lastInteraction = millis();
  }

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
