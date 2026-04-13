#include <Arduino.h>
#include <avr/wdt.h>
#include "menu.h"
#include "bame_state.h"

#define BAME_VERSION   "2.0-wip"
#define CAPACITY_MIN   1.0f
#define CAPACITY_MAX   500.0f

// Flat menu — only items the user actually needs to touch.
// Cell count + wiring are compile-time. V min/V max are gone (no voltage SOC
// in v2). Eco mode is gone (always-on). "Battery full" is the manual reset
// for the SOC integrator when the auto-detector hasn't fired (e.g. just after
// a slow charge, or LOAD install where you trust your eyes).
enum MenuItem {
  ITEM_CAP,        // user-set sticker capacity (Ah)
  ITEM_FULL,       // declare battery full → SOC = 100%
  ITEM_RESET,      // wipe all EEPROM and reboot
  ITEM_INFO_V,     // read-only: voltage + SOC%
  SYS_COUNT
};


void settingsMenu() {
  uint8_t sel = 0;
  int8_t  editing = -1;
  float   tmpCap = batteryCapacityNom;
  bool    tmpConfirm = false;

  while (true) {
    display.clearDisplay();
    gfx.drawTitle(F("BaMe v" BAME_VERSION));

    // Read-only Info row (last position): voltage + SOC%
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
      display.print(F("V "));
      display.print((int)socPercent());
      display.print('%');
      display.setTextColor(SSD1306_WHITE);
    }

    char buf[10];

    // Capacity (editable)
    int capVal = (int)(editing == ITEM_CAP ? tmpCap : batteryCapacityNom);
    itoa(capVal, buf, 10);
    strcat(buf, "Ah");
    gfx.drawMenuItem(ITEM_CAP, ' ', F("Capacity"), buf,
                     sel == ITEM_CAP, editing == ITEM_CAP);

    // Battery full (action with confirmation)
    gfx.drawMenuItem(ITEM_FULL, ' ', F("Battery full"),
                     editing == ITEM_FULL ? (tmpConfirm ? "YES" : "NO") : "",
                     sel == ITEM_FULL, editing == ITEM_FULL);

    // Reset ALL (action with confirmation)
    gfx.drawMenuItem(ITEM_RESET, ' ', F("Reset ALL"),
                     editing == ITEM_RESET ? (tmpConfirm ? "YES" : "NO") : "",
                     sel == ITEM_RESET, editing == ITEM_RESET);

    display.display();

    Button b = readButtonDebounced();
    if (b == BTN_NONE) { delay(50); continue; }

    if (editing >= 0) {
      switch (b) {
        case BTN_UP:
          if (editing == ITEM_CAP) {
            tmpCap += 1; if (tmpCap > CAPACITY_MAX) tmpCap = CAPACITY_MAX;
          } else {
            tmpConfirm = !tmpConfirm;
          }
          break;
        case BTN_DOWN:
          if (editing == ITEM_CAP) {
            tmpCap -= 1; if (tmpCap < CAPACITY_MIN) tmpCap = CAPACITY_MIN;
          } else {
            tmpConfirm = !tmpConfirm;
          }
          break;
        case BTN_CENTER:
          if (editing == ITEM_CAP) {
            setCapacityNom(tmpCap);
            saveNomEEPROM();
          } else if (editing == ITEM_FULL && tmpConfirm) {
            declareBatteryFull(millis());
          } else if (editing == ITEM_RESET && tmpConfirm) {
            resetAllEEPROM();
            wdt_enable(WDTO_15MS);
            while (true) {}
          }
          editing = -1;
          tmpConfirm = false;
          break;
        case BTN_LEFT:
          editing = -1;
          tmpConfirm = false;
          tmpCap = batteryCapacityNom;
          break;
        default: break;
      }
    } else {
      switch (b) {
        case BTN_UP:    sel = (sel == 0) ? SYS_COUNT - 1 : sel - 1; break;
        case BTN_DOWN:  sel = (sel + 1) % SYS_COUNT; break;
        case BTN_CENTER:
          if (sel == ITEM_INFO_V) break;  // info is read-only
          editing = sel;
          tmpCap = batteryCapacityNom;
          tmpConfirm = false;
          break;
        case BTN_LEFT: waitButtonRelease(); return;  // exit menu
        default: break;
      }
    }
    waitButtonRelease();
  }
}
