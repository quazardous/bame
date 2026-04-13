#include <Arduino.h>
#include "display.h"
#include "bame_state.h"

// v2 layout: gauge, big Ah + V, watts (smoothed) + raw current, HH:MM or
// capacity at rest. A '?' next to Ah marks SOC uncertainty (LOAD-mode
// invisible partial charge); a '*' next to capacity marks "not yet learned".

#define MIN_BATTERY_V       1.0f
#define ACTIVE_CURRENT      0.5f
#define LFP_CELL_TOP_REST   3.40f


void updateDisplay() {
  display.clearDisplay();

  if (!batteryPresent) {
    gfx.drawGauge(0);
    display.setTextSize(2);
    display.setCursor(4, BLUE_Y + 12);
    if ((millis() / 500) % 2) display.print(F("No Battery"));
    display.display();
    return;
  }

  float soc = socPercent();
  gfx.drawGauge(soc);

  // Line 1: Ah (left, big) + voltage (right, big), with '?' if uncertain.
  int ahInt = (int)(coulombCount / 3600.0);
  if (ahInt < 0) ahInt = 0;
  uint8_t ahDigits = (ahInt >= 100) ? 3 : (ahInt >= 10) ? 2 : 1;
  display.setTextSize(2);
  display.setCursor(0, BLUE_Y + 2);
  display.print(ahInt);
  display.setCursor(SCREEN_W - 54, BLUE_Y + 2);
  display.print(voltage, 1);
  display.setTextSize(1);
  display.setCursor(ahDigits * 12, BLUE_Y + 2);
  display.print(F("Ah"));
  if (socUncertain) {
    display.setCursor(ahDigits * 12, BLUE_Y + 10);
    display.print('?');
  }
  display.setCursor(SCREEN_W - 6, BLUE_Y + 2);
  display.print(F("V"));

  // Line 2: power (smoothed) + raw current
  float iForPower = cAvgInit ? cAvg : current;
  display.setCursor(0, BLUE_Y + 22);
  display.print((int)abs(iForPower * voltage));
  display.print(F("W"));
  {
    int ci = (int)abs(current);
    uint8_t alen = 4;
    if (ci >= 10) alen++;
    if (ci >= 100) alen++;
    if (current < 0) alen++;
    display.setCursor(SCREEN_W - alen * 6, BLUE_Y + 22);
    display.print(current, 1);
    display.print(F("A"));
  }

  // Line 3: HH:MM remaining (active) or capacity (at rest)
  int16_t ty = BLUE_Y + 37;
  float iAuto = cAvgInit ? cAvg : current;
  if (iAuto > ACTIVE_CURRENT) {
    float hoursLeft = (coulombCount / 3600.0) / iAuto;
    hoursLeft = constrain(hoursLeft, 0.0f, 99.9f);
    int h = (int)hoursLeft;
    int m = (int)((hoursLeft - h) * 60);
    display.fillTriangle(0, ty + 3, 6, ty, 6, ty + 6, SSD1306_WHITE);
    display.setCursor(10, ty);
    if (h < 10) display.print('0');
    display.print(h);
    display.print(':');
    if (m < 10) display.print('0');
    display.print(m);
  } else if (iAuto < -ACTIVE_CURRENT) {
    float remaining = (capacityAs() - coulombCount) / 3600.0f;
    if (remaining < 0) remaining = 0;
    float hoursLeft = remaining / (-iAuto);
    hoursLeft = constrain(hoursLeft, 0.0f, 99.9f);
    int h = (int)hoursLeft;
    int m = (int)((hoursLeft - h) * 60);
    display.fillTriangle(6, ty + 3, 0, ty, 0, ty + 6, SSD1306_WHITE);
    display.setCursor(10, ty);
    if (h < 10) display.print('0');
    display.print(h);
    display.print(':');
    if (m < 10) display.print('0');
    display.print(m);
  } else {
    display.setCursor(0, ty);
    display.print((int)batteryCapacityAh);
    display.print(F("Ah"));
    if (!capacityLearned) {
      display.setCursor(28, ty);
      display.print('*');
    }
  }

  // Bottom right: charging icon when voltage is at top OCV (post-charge state)
  if ((voltage / cellCount) >= LFP_CELL_TOP_REST) {
    gfx.drawChargingBattery(106, ty, true);
  }

  display.display();
}
