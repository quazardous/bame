#include <Arduino.h>
#include "display.h"
#include "bame_state.h"

// Constants reused from the original main.cpp. Duplicated here verbatim so
// the split is mechanical — moving them into a config.h is the next step.
#define MIN_BATTERY_V       1.0
#define ACTIVE_CURRENT      0.5
#define LFP_CELL_CHARGE     3.40
#define DISPLAY_INTERVAL_MS 500
#define FLAT_COUNTDOWN_MS   60000UL
#define CAL_INITIAL_TIME_MS 120000
#define CAL_MIN_COULOMBS    500.0


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
