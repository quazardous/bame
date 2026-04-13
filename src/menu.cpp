#include <Arduino.h>
#include <EEPROM.h>
#include <avr/wdt.h>
#include "menu.h"
#include "bame_state.h"

// Local constants mirrored from the original main.cpp (move to config.h later)
#define BAME_VERSION        "2.0-wip"
#define CAPACITY_MIN        1.0
#define CAPACITY_MAX        500.0
#define LFP_CELL_FULL       3.65
#define LFP_CELL_EMPTY      2.50
#define LFP_CELL_CHARGE     3.40


// --- SYSTEM menu (flat) ---
enum MenuItem {
  ITEM_CAP,
#if !BAME_DEV
  ITEM_VMIN,
  ITEM_VMAX,
  ITEM_ECO,
#endif
  ITEM_RESET,
  ITEM_INFO_V,    // read-only: current voltage + min observed
  SYS_COUNT
};

void settingsMenu() {
  uint8_t sel = 0;
  int8_t editing = -1;

  float tmpCap = batteryCapacityNom;
#if !BAME_DEV
  float tmpVmin = vMinUtile;
  float tmpVmax = vMaxUtile;
#endif
  bool tmpConfirm = false;
#if !BAME_DEV
  bool tmpSleep = autoDeepSleep;
#endif

  while (true) {
    display.clearDisplay();
    gfx.drawTitle(F("Bame v" BAME_VERSION));

    // Info row (display only): current voltage + min observed
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
      display.print(F("V min:"));
      if (bufMin > 0) display.print(bufMin, 2);
      else display.print('-');
      display.setTextColor(SSD1306_WHITE);
    }

    char buf[10];

    // Capacity
    int capVal = (int)(editing == ITEM_CAP ? tmpCap : batteryCapacityNom);
    itoa(capVal, buf, 10);
    strcat(buf, "Ah");
    gfx.drawMenuItem(ITEM_CAP, ' ', F("Capacity"), buf, sel == ITEM_CAP, editing == ITEM_CAP);

#if !BAME_DEV
    // V min utile (with observed min appended)
    dtostrf(editing == ITEM_VMIN ? tmpVmin : vMinUtile, 0, 1, buf);
    {
      char *p = buf + strlen(buf);
      *p++ = 'V';
      if (editing != ITEM_VMIN && bufMin > 0) {
        *p++ = '/';
        dtostrf(bufMin, 0, 1, p);
      } else *p = 0;
    }
    gfx.drawMenuItem(ITEM_VMIN, ' ', F("V min"), buf, sel == ITEM_VMIN, editing == ITEM_VMIN);

    // V max utile
    dtostrf(editing == ITEM_VMAX ? tmpVmax : vMaxUtile, 0, 1, buf);
    strcat(buf, "V");
    gfx.drawMenuItem(ITEM_VMAX, ' ', F("V max"), buf, sel == ITEM_VMAX, editing == ITEM_VMAX);
    // Eco mode
    gfx.drawMenuItem(ITEM_ECO, ' ', F("Eco mode"),
      (editing == ITEM_ECO ? tmpSleep : autoDeepSleep) ? "ON" : "OFF",
      sel == ITEM_ECO, editing == ITEM_ECO);
#endif

    // Reset ALL
    gfx.drawMenuItem(ITEM_RESET, ' ', F("Reset ALL"),
      editing == ITEM_RESET ? (tmpConfirm ? "YES" : "NO") : "",
      sel == ITEM_RESET, editing == ITEM_RESET);

    display.display();

    Button b = readButtonDebounced();
    if (b == BTN_NONE) { delay(50); continue; }

    if (editing >= 0) {
      switch (b) {
        case BTN_UP:
          if (editing == ITEM_CAP) { tmpCap += 1; if (tmpCap > CAPACITY_MAX) tmpCap = CAPACITY_MAX; }
#if !BAME_DEV
          else if (editing == ITEM_VMIN) { tmpVmin += 0.05; if (tmpVmin > cellCount * LFP_CELL_CHARGE) tmpVmin = cellCount * LFP_CELL_CHARGE; }
          else if (editing == ITEM_VMAX) { tmpVmax += 0.05; if (tmpVmax > cellCount * LFP_CELL_FULL) tmpVmax = cellCount * LFP_CELL_FULL; }
          else if (editing == ITEM_ECO) { tmpSleep = !tmpSleep; }
#endif
          else if (editing == ITEM_RESET) { tmpConfirm = !tmpConfirm; }
          break;
        case BTN_DOWN:
          if (editing == ITEM_CAP) { tmpCap -= 1; if (tmpCap < CAPACITY_MIN) tmpCap = CAPACITY_MIN; }
#if !BAME_DEV
          else if (editing == ITEM_VMIN) { tmpVmin -= 0.05; if (tmpVmin < cellCount * LFP_CELL_EMPTY) tmpVmin = cellCount * LFP_CELL_EMPTY; }
          else if (editing == ITEM_VMAX) { tmpVmax -= 0.05; if (tmpVmax < cellCount * LFP_CELL_CHARGE) tmpVmax = cellCount * LFP_CELL_CHARGE; }
          else if (editing == ITEM_ECO) { tmpSleep = !tmpSleep; }
#endif
          else if (editing == ITEM_RESET) { tmpConfirm = !tmpConfirm; }
          break;
        case BTN_CENTER:
          if (editing == ITEM_CAP) {
            batteryCapacityNom = tmpCap;
            saveNomToEEPROM();
            setCapacity(batteryCapacityNom);
            saveCapToEEPROM();
          }
#if !BAME_DEV
          else if (editing == ITEM_VMIN) {
            vMinUtile = tmpVmin;
          } else if (editing == ITEM_VMAX) {
            vMaxUtile = tmpVmax;
          }
          else if (editing == ITEM_ECO) {
            autoDeepSleep = tmpSleep;
            saveAutoSleepToEEPROM();
          }
#endif
          else if (editing == ITEM_RESET) {
            if (tmpConfirm) {
              for (uint16_t i = 0; i < E2END + 1; i++) EEPROM.write(i, 0xFF);
              wdt_enable(WDTO_15MS);
              while (true);
            }
          }
          editing = -1;
          break;
        case BTN_LEFT:
          tmpCap = batteryCapacityNom;
#if !BAME_DEV
          tmpVmin = vMinUtile;
          tmpVmax = vMaxUtile;
          tmpSleep = autoDeepSleep;
#endif
          tmpConfirm = false;
          editing = -1;
          break;
        default: break;
      }
    } else {
      switch (b) {
        case BTN_UP: sel = (sel == 0) ? SYS_COUNT - 1 : sel - 1; break;
        case BTN_DOWN: sel = (sel + 1) % SYS_COUNT; break;
        case BTN_CENTER:
          if (sel == ITEM_INFO_V) break;  // info item: no action
          editing = sel;
          tmpCap = batteryCapacityNom;
#if !BAME_DEV
          tmpVmin = vMinUtile;
          tmpVmax = vMaxUtile;
          tmpSleep = autoDeepSleep;
#endif
          tmpConfirm = false;
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
