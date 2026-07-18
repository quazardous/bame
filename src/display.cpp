#include <Arduino.h>
#include "display.h"
#include "bame_state.h"

// v2 layout: gauge, big Ah + V, watts (smoothed) + raw current, HH:MM or
// capacity at rest. A '?' next to Ah marks SOC uncertainty (LOAD-mode
// invisible partial charge); a '*' next to capacity marks "not yet learned".

#define MIN_BATTERY_V       1.0f
#define ACTIVE_CURRENT      0.5f
#define LFP_CELL_TOP_REST   3.40f
// Slow (τ ≈ 1 h) autonomy gate. Much lower than ACTIVE_CURRENT: a duty-cycled
// fridge averages only a few hundred mA over the hour, yet that's a real draw
// with a real autonomy worth showing.
#define SLOW_AUTONOMY_MIN   0.1f


// Print a duration at the current cursor. Below 100 h it's HH:MM as before;
// above, it switches to days ("4.2d", then "18d") so a low duty-cycled draw
// isn't stuck at the old 99:59 ceiling. Stays within 6 chars either way, so it
// fits the tight bottom-right slot. Capped at 999 d (a regime no real pack
// reaches — 80 Ah would need ~3 mA).
static void printDuration(float hours) {
  if (hours < 100.0f) {
    int h = (int)hours;
    int m = (int)((hours - h) * 60);
    if (h < 10) display.print('0');
    display.print(h);
    display.print(':');
    if (m < 10) display.print('0');
    display.print(m);
  } else {
    float days = hours / 24.0f;
    if (days > 999.0f) days = 999.0f;
    if (days < 10.0f) display.print(days, 1);   // "4.2"
    else              display.print((int)days); // "18"
    display.print('d');
  }
}

void updateDisplay() {
  display.clearDisplay();

  if (!batteryPresent) {
    gfx.drawGauge(0);
    display.setTextSize(2);
    display.setCursor(4, BLUE_Y + 12);
    if ((millis() / 500) % 2) display.print(F("No Battery"));

    // Firmware version: fixed (non-blinking), small font, bottom-right.
    display.setTextSize(1);
    int16_t bx, by; uint16_t bw, bh;
    display.getTextBounds(F("v" BAME_VERSION), 0, 0, &bx, &by, &bw, &bh);
    display.setCursor(SCREEN_W - bw, SCREEN_H - 8);
    display.print(F("v" BAME_VERSION));

    display.display();
    return;
  }

  float soc = socPercent();
  gfx.drawGauge(soc);

  // Line 1: Ah (left, big) + voltage (right, big), with '?' if uncertain.
  // Round (not truncate) so tiny sub-1Ah drift between full-event re-syncs
  // doesn't show "79Ah" when the user is obviously at 100%.
  int ahInt = (int)((coulombCount / 3600.0) + 0.5f);
  if (ahInt < 0) ahInt = 0;
  if (ahInt > (int)batteryCapacityAh) ahInt = (int)batteryCapacityAh;
  uint8_t ahDigits = (ahInt >= 100) ? 3 : (ahInt >= 10) ? 2 : 1;
  // Right of line 1: the raw current, big (it's the number you watch). Width is
  // computed so it stays right-aligned: "d.d" plus a digit per decade and one
  // for a charge's minus sign, then the small 'A' suffix.
  int   ciAbs = (int)abs(current);
  uint8_t cLen = 3;                      // "d.d"
  if (ciAbs >= 10)  cLen++;
  if (ciAbs >= 100) cLen++;
  if (current < 0)  cLen++;              // '-' when charging
  display.setTextSize(2);
  display.setCursor(0, BLUE_Y + 2);
  display.print(ahInt);
  display.setCursor(SCREEN_W - 6 - cLen * 12, BLUE_Y + 2);
  display.print(current, 1);
  display.setTextSize(1);
  display.setCursor(ahDigits * 12, BLUE_Y + 2);
  display.print(F("Ah"));
  if (socUncertain) {
    display.setCursor(ahDigits * 12, BLUE_Y + 10);
    display.print('?');
  }
  // Provisional capacity being measured this cycle: peak Ah delivered so far,
  // blinking, next to the '?', until a cycle confirms the real capacity. It's a
  // lower bound ("at least this big") that grows toward the true capacity as the
  // pack empties — an indication only, the big Ah/gauge are unchanged.
  // The big current is right-aligned with a variable width and is 16 px tall, so
  // it reaches down into this y+10 row. Only draw the hint if it actually fits
  // to the left of it (3-digit Ah + 3-digit hint + "-30.0" would otherwise
  // overlap). Dropping the hint is the right call: it's the optional decoration.
  if (!capacityLearned && deliveredAh >= 1.0f && (millis() / 500) % 2) {
    int dHint = (int)(deliveredAh + 0.5f);
    uint8_t hLen = (dHint >= 100) ? 3 : (dHint >= 10) ? 2 : 1;
    int16_t hx  = ahDigits * 12 + (socUncertain ? 8 : 0);
    if (hx + hLen * 6 <= SCREEN_W - 6 - cLen * 12 - 2) {   // 2 px clearance
      display.setCursor(hx, BLUE_Y + 10);
      display.print(dHint);
    }
  }
  display.setCursor(SCREEN_W - 6, BLUE_Y + 2);
  display.print(F("A"));

  // Line 2: power (smoothed) left + voltage (small) right
  float iForPower = cAvgInit ? cAvg : current;
  display.setCursor(0, BLUE_Y + 22);
  display.print((int)abs(iForPower * voltage));
  display.print(F("W"));
  {
    uint8_t vLen = (voltage >= 10.0f) ? 5 : 4;   // "13.2V" / "9.9V"
    display.setCursor(SCREEN_W - vLen * 6, BLUE_Y + 22);
    display.print(voltage, 1);
    display.print(F("V"));
  }

  // Line 3 left: HH:MM remaining (active) or capacity (at rest), on the
  // responsive 30 s average — the "right now" figure.
  int16_t ty = BLUE_Y + 37;
  float iAuto = cAvgInit ? cAvg : current;
  if (iAuto > ACTIVE_CURRENT) {
    float hoursLeft = (coulombCount / 3600.0) / iAuto;
    display.fillTriangle(0, ty + 3, 6, ty, 6, ty + 6, SSD1306_WHITE);
    display.setCursor(10, ty);
    printDuration(hoursLeft);
  } else if (iAuto < -ACTIVE_CURRENT) {
    float remaining = (capacityAs() - coulombCount) / 3600.0f;
    if (remaining < 0) remaining = 0;
    float hoursLeft = remaining / (-iAuto);
    display.fillTriangle(6, ty + 3, 0, ty, 0, ty + 6, SSD1306_WHITE);
    display.setCursor(10, ty);
    printDuration(hoursLeft);
  } else {
    display.setCursor(0, ty);
    display.print((int)batteryCapacityAh);
    display.print(F("Ah"));
    if (!capacityLearned) {
      display.setCursor(28, ty);
      display.print('*');
    }
  }

  // Bottom right: the SAME autonomy but on the slow average (τ ≈ 1 h), marked
  // '~'. For an intermittent load (fridge compressor) this reflects the real
  // duty-cycled draw, so it stays steady and is the figure to plan on — while
  // the left one tracks what's happening right now. Its threshold is far lower
  // than ACTIVE_CURRENT: a duty-cycled fridge averages only a few hundred mA,
  // which is still a perfectly real draw with a real autonomy.
  // The LOAD-mode charging icon lives here too and takes precedence.
  if (chargingExternal) {
    gfx.drawChargingBattery(106, ty, true);
  } else {
    float iSlow = cAvgInit ? cAvgSlow : current;
    if (iSlow > SLOW_AUTONOMY_MIN) {
      display.setCursor(SCREEN_W - 36, ty);
      display.print('~');   // both readouts are on the slow (τ ≈ 1 h) average
      // Alternate every 3 s between the autonomy and the average consumption in
      // watts — two ways to read the same duty-cycled draw (the planning figure).
      if ((millis() / 3000) % 2) {
        display.print((int)(iSlow * voltage));
        display.print('W');
      } else {
        printDuration((coulombCount / 3600.0) / iSlow);
      }
    }
  }

  display.display();
}
