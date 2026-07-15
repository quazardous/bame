// ===========================================================================
// BaMe INA226 diagnostic helper
//
// A standalone firmware (separate PlatformIO env) that checks the INA226 is
// wired and talking, and shows live readings on the OLED — use it when the
// main firmware is stuck on "No Battery" (which happens whenever the bus
// voltage reads < 1 V, i.e. the INA226 isn't seen or VBUS isn't connected).
//
// Build & flash:
//   pio run -e diag-prod -t upload      # ATmega328PB via USBasp
//   pio run -e diag-nano -t upload      # Arduino Nano via USB
//
// On screen:
//   I2C 0x40  : OK / NOT FOUND     ← does the INA226 ACK on the bus?
//   ID        : ok / bad hex       ← manufacturer 0x5449 + die 0x2260
//   Vbus      : bus voltage        ← must match your pack (~13 V / ~26 V)
//   Vshunt    : shunt drop (mV)    ← nonzero only when current flows
//   I         : current (A)        ← sign: + discharge, - charge
// Serial (Nano) mirrors it at 115200 and lists every I2C address found.
// ===========================================================================

#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <INA226.h>

#define OLED_ADDR        0x3C
#define INA226_ADDR      0x40
#define SHUNT_RESISTANCE 0.0025f
#define MAX_CURRENT      30.0f

Adafruit_SSD1306 display(128, 64, &Wire, -1);
INA226 ina(INA226_ADDR, &Wire);

static bool     haveOled = false;
static bool     inaOk    = false;
static uint16_t mfgId    = 0;
static uint16_t dieId    = 0;
static int      shuntErr = 0;
static char     foundAddrs[30] = "";   // hex list of every I2C address that ACKs

static void i2cScan() {
  Serial.println(F("I2C scan:"));
  foundAddrs[0] = '\0';
  uint8_t n = 0;
  char* p = foundAddrs;
  for (uint8_t a = 0x03; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("  found 0x")); Serial.println(a, HEX);
      n++;
      if (p - foundAddrs < (int)sizeof(foundAddrs) - 4) {
        if (p != foundAddrs) *p++ = ' ';
        p += sprintf(p, "%02X", a);
      }
    }
  }
  if (!n) { strcpy(foundAddrs, "none"); Serial.println(F("  (nothing responded)")); }
}

void setup() {
  Serial.begin(115200);
  Serial.println(F("\nBaMe INA226 diagnostic"));

  Wire.begin();
  haveOled = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (haveOled) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
  }

  i2cScan();

  ina.begin();
  inaOk = ina.isConnected();       // reads manufacturer ID under the hood
  mfgId = ina.getManufacturerID(); // expect 0x5449
  dieId = ina.getDieID();          // expect 0x2260
  shuntErr = ina.setMaxCurrentShunt(MAX_CURRENT, SHUNT_RESISTANCE);

  Serial.print(F("INA226 connected: ")); Serial.println(inaOk ? F("YES") : F("NO"));
  Serial.print(F("  mfgID=0x")); Serial.print(mfgId, HEX);
  Serial.print(F(" dieID=0x")); Serial.print(dieId, HEX);
  Serial.print(F(" shuntCfg=")); Serial.println(shuntErr);
}

void loop() {
  float vbus  = ina.getBusVoltage();          // V
  float vsh   = ina.getShuntVoltage() * 1000; // mV
  float amps  = ina.getCurrent();             // A

  // --- Serial ---
  Serial.print(F("Vbus=")); Serial.print(vbus, 3);
  Serial.print(F("V  Vshunt=")); Serial.print(vsh, 2);
  Serial.print(F("mV  I=")); Serial.print(amps, 3); Serial.println(F("A"));

  // --- OLED: compact status line + big live voltage & consumption ---
  if (haveOled) {
    display.clearDisplay();

    // Status line (small).
    display.setTextSize(1);
    display.setCursor(0, 0);
    if (inaOk) {
      display.print(F("INA226 0x40 OK"));
    } else {
      display.print(F("INA226 NOT FOUND"));
    }

    if (inaOk) {
      // Voltage — big.
      display.setTextSize(2);
      display.setCursor(0, 14);
      display.print(vbus, 2); display.print(F("V"));

      // Consumption — big (current); power + shunt small underneath.
      display.setCursor(0, 36);
      display.print(amps, 2); display.print(F("A"));

      display.setTextSize(1);
      display.setCursor(0, 56);
      display.print((int)(vbus * amps)); display.print(F("W  sh"));
      display.print(vsh, 1); display.print(F("mV"));

      if (vbus < 1.0f) {  // chip fine but no bus voltage → VBUS wiring
        display.setCursor(0, 14);
        display.setTextSize(1);
        display.print(F("Vbus ~0 -> check the"));
        display.setCursor(0, 24);
        display.print(F("VBUS pin wiring"));
      }
    } else {
      // INA226 didn't ACK at 0x40 — show what IS on the bus so you can tell
      // "not powered / not wired" (only 3C) from "wrong address" (e.g. 3C 44).
      display.setCursor(0, 14); display.print(F("I2C found:"));
      display.setTextSize(2);
      display.setCursor(0, 26); display.print(foundAddrs);
      display.setTextSize(1);
      display.setCursor(0, 48); display.print(F("INA226 not at 0x40"));
      display.setCursor(0, 56); display.print(F("chk VCC/SDA/SCL/addr"));
    }
    display.display();
  }

  delay(300);
}
