#pragma once
#include <Adafruit_SSD1306.h>

// Bicolor SSD1306 128x64 screen layout
// Yellow zone: rows 0-15 (16px)
// Blue zone: rows 16-63 (48px)

#define SCREEN_W 128
#define SCREEN_H 64
#define YELLOW_H 16
#define BLUE_Y   16
#define BLUE_H   48

class BameGFX {
public:
  BameGFX(Adafruit_SSD1306 &disp);

  // Gauge bar in yellow zone with XOR text (builtin font size 2)
  // percent: 0-100, resolution = 126 columns
  // label: if non-null, display this text instead of %
  void drawGauge(float percent, const __FlashStringHelper *label = nullptr);

  // Centered title in yellow zone (font size 1, auto uppercase)
  void drawTitle(const __FlashStringHelper *title);

  // Menu item with type prefix and optional value
  void drawMenuItem(uint8_t row, char prefix, const __FlashStringHelper* label,
                    const char* value = nullptr, bool selected = false, bool editing = false);

  // Animated charging battery icon
  void drawChargingBattery(int16_t x, int16_t y);

  // Call in loop() to advance animations
  void tick();

  // Draw number + "%" in XOR (builtin font, centered)
  void drawPercentXOR(int percent, int16_t y, int16_t zone_x = 0, int16_t zone_w = SCREEN_W);

private:
  Adafruit_SSD1306 &_disp;
  uint8_t _frame;
};
