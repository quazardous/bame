#include "BameGFX.h"

// Adafruit GFX builtin font: 6x8 per char, size 2 = 12x16

#define CHAR_W 12  // 6 * 2
#define CHAR_H 16  // 8 * 2

BameGFX::BameGFX(Adafruit_SSD1306 &disp) : _disp(disp), _frame(0) {}

void BameGFX::drawPercentXOR(int percent, int16_t y, int16_t zone_x, int16_t zone_w) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;

  char buf[5];
  itoa(percent, buf, 10);
  uint8_t len = strlen(buf);
  buf[len] = '%';
  buf[len + 1] = '\0';

  int16_t totalW = (len + 1) * CHAR_W;
  int16_t x = zone_x + (zone_w - totalW) / 2;

  _disp.setTextSize(2);
  _disp.setTextColor(SSD1306_INVERSE, SSD1306_INVERSE);
  _disp.setCursor(x, y);
  _disp.print(buf);
}


void BameGFX::drawGauge(float percent, const __FlashStringHelper *label) {
  if (percent < 0.0f) percent = 0.0f;
  if (percent > 100.0f) percent = 100.0f;

  // 1px border
  _disp.drawRect(0, 0, SCREEN_W, YELLOW_H, SSD1306_WHITE);

  // Inner area: 126 x 14 pixels
  const int16_t bx = 1, by = 1, bw = 126, bh = 14;

  // 126 columns for 0-100%
  int cols = (int)(percent * bw / 100.0f);

  if (cols > 0) {
    _disp.fillRect(bx, by, cols, bh, SSD1306_WHITE);
  }

  // XOR text centered on yellow zone
  if (label) {
    int16_t w = strlen_P((const char*)label) * CHAR_W;
    int16_t x = (SCREEN_W - w) / 2;
    _disp.setTextSize(2);
    _disp.setTextColor(SSD1306_INVERSE, SSD1306_INVERSE);
    _disp.setCursor(x, 0);
    _disp.print(label);
  } else {
    drawPercentXOR((int)percent, 0, 0, SCREEN_W);
  }
}

void BameGFX::drawTitle(const __FlashStringHelper *title) {
  const char *p = (const char*)title;
  uint8_t len = strlen_P(p);
  int16_t x = (SCREEN_W - len * 6) / 2;
  _disp.setTextSize(1);
  _disp.setTextColor(SSD1306_WHITE);
  _disp.setCursor(x, 4);
  char c;
  while ((c = pgm_read_byte(p++))) _disp.print((char)toupper(c));
}

void BameGFX::drawMenuItem(uint8_t row, char prefix, const __FlashStringHelper* label,
                           const char* value, bool selected, bool editing) {
  int16_t y = BLUE_Y + row * 8;
  _disp.setTextSize(1);

  if (selected) {
    _disp.fillRect(0, y, SCREEN_W, 8, SSD1306_WHITE);
    _disp.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
  } else {
    _disp.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
  }

  _disp.setCursor(0, y);
  _disp.print(prefix);
  _disp.print(' ');
  _disp.print(label);

  if (value && value[0]) {
    uint8_t vlen = strlen(value);
    if (editing) {
      int16_t vw = (vlen + 2) * 6;
      _disp.setCursor(SCREEN_W - vw, y);
      _disp.print('[');
      _disp.print(value);
      _disp.print(']');
    } else {
      int16_t vw = vlen * 6;
      _disp.setCursor(SCREEN_W - vw, y);
      _disp.print(value);
    }
  }
  _disp.setTextColor(SSD1306_WHITE);
}

void BameGFX::tick() {
  _frame++;
}

void BameGFX::drawChargingBattery(int16_t x, int16_t y) {
  int16_t bw = 16, bh = 10;

  // Body
  _disp.drawRect(x, y, bw, bh, SSD1306_WHITE);
  // Tip (+ terminal)
  _disp.fillRect(x + bw, y + 2, 2, bh - 4, SSD1306_WHITE);

  // Animated fill: 4 levels cycling
  uint8_t level = (_frame / 3) % 5; // 0 = empty, 1-4 = levels
  int16_t fillW = level * 3; // 0, 3, 6, 9, 12 pixels
  if (fillW > 0) {
    _disp.fillRect(x + 2, y + 2, fillW, bh - 4, SSD1306_WHITE);
  }
}
