#include "BameGFX.h"

// Font builtin Adafruit GFX : 6x8 par char, size 2 = 12x16

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

void BameGFX::drawNumberXOR(int number, int16_t y, int16_t zone_x, int16_t zone_w) {
  char buf[7];
  itoa(number, buf, 10);

  int16_t totalW = strlen(buf) * CHAR_W;
  int16_t x = zone_x + (zone_w - totalW) / 2;

  _disp.setTextSize(2);
  _disp.setTextColor(SSD1306_INVERSE, SSD1306_INVERSE);
  _disp.setCursor(x, y);
  _disp.print(buf);
}

void BameGFX::drawGauge(float percent, const __FlashStringHelper *label) {
  if (percent < 0.0f) percent = 0.0f;
  if (percent > 100.0f) percent = 100.0f;

  // Bordure 1px
  _disp.drawRect(0, 0, SCREEN_W, YELLOW_H, SSD1306_WHITE);

  // Zone interieure : 126 x 14 pixels
  const int16_t bx = 1, by = 1, bw = 126, bh = 14;

  // 126 colonnes pour 0-100%
  int cols = (int)(percent * bw / 100.0f);

  if (cols > 0) {
    _disp.fillRect(bx, by, cols, bh, SSD1306_WHITE);
  }

  // Texte XOR centre sur la zone jaune
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
  _disp.setTextSize(1);
  _disp.setTextColor(SSD1306_WHITE);
  int16_t w = strlen_P((const char*)title) * 6;
  int16_t x = (SCREEN_W - w) / 2;
  _disp.setCursor(x, 4);
  _disp.print(title);
}

void BameGFX::drawText(uint8_t row, const char *text, uint8_t size) {
  _disp.setTextSize(size);
  _disp.setTextColor(SSD1306_WHITE);
  int16_t w = strlen(text) * 6 * size;
  int16_t x = (SCREEN_W - w) / 2;
  _disp.setCursor(x, BLUE_Y + row * 9);
  _disp.print(text);
}

void BameGFX::drawText(uint8_t row, const __FlashStringHelper *text, uint8_t size) {
  _disp.setTextSize(size);
  _disp.setTextColor(SSD1306_WHITE);
  int16_t w = strlen_P((const char*)text) * 6 * size;
  int16_t x = (SCREEN_W - w) / 2;
  _disp.setCursor(x, BLUE_Y + row * 9);
  _disp.print(text);
}

void BameGFX::drawMenuItem(uint8_t row, char prefix, const __FlashStringHelper* label,
                           const char* value, bool selected, bool editing) {
  int16_t y = BLUE_Y + 1 + row * 10;
  _disp.setTextSize(1);

  if (selected) {
    _disp.fillRect(0, y - 1, SCREEN_W, 10, SSD1306_WHITE);
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

void BameGFX::drawCalSpinner(int16_t x, int16_t y) {
  uint8_t phase = (millis() / 200) % 4;
  switch (phase) {
    case 0: _disp.drawFastVLine(x, y - 2, 5, SSD1306_WHITE); break;
    case 1: _disp.drawLine(x - 2, y + 2, x + 2, y - 2, SSD1306_WHITE); break;
    case 2: _disp.drawFastHLine(x - 2, y, 5, SSD1306_WHITE); break;
    case 3: _disp.drawLine(x - 2, y - 2, x + 2, y + 2, SSD1306_WHITE); break;
  }
}

void BameGFX::drawProgress(uint8_t row, int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  int16_t y = BLUE_Y + row * 9;
  int w = (percent * 124) / 100;
  _disp.drawRect(0, y, SCREEN_W, 8, SSD1306_WHITE);
  if (w > 0) _disp.fillRect(2, y + 1, w, 6, SSD1306_WHITE);
}

void BameGFX::tick() {
  _frame++;
}

void BameGFX::drawFlowArrows(int16_t x, int16_t y, uint8_t h) {
  // 4 chevrons > espaces de 10px, animes par decalage
  int8_t offset = _frame % 10;

  for (int i = 0; i < 4; i++) {
    int16_t ax = x + i * 10 + offset;
    int16_t midY = y + h / 2;
    int16_t halfH = h / 2;

    for (int r = 0; r < halfH; r++) {
      _disp.drawPixel(ax + r, midY - halfH + r, SSD1306_WHITE);
      _disp.drawPixel(ax + r, midY + halfH - r - 1, SSD1306_WHITE);
    }
  }
}

void BameGFX::drawChargingBattery(int16_t x, int16_t y) {
  int16_t bw = 16, bh = 10;

  // Corps
  _disp.drawRect(x, y, bw, bh, SSD1306_WHITE);
  // Tip (borne +)
  _disp.fillRect(x + bw, y + 2, 2, bh - 4, SSD1306_WHITE);

  // Remplissage anime : 4 niveaux qui cyclent
  uint8_t level = (_frame / 3) % 5; // 0 = vide, 1-4 = niveaux
  int16_t fillW = level * 3; // 0, 3, 6, 9, 12 pixels
  if (fillW > 0) {
    _disp.fillRect(x + 2, y + 2, fillW, bh - 4, SSD1306_WHITE);
  }
}
