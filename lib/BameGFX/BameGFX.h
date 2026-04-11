#pragma once
#include <Adafruit_SSD1306.h>

// Layout ecran bicolor SSD1306 128x64
// Zone jaune : lignes 0-15 (16px)
// Zone bleue : lignes 16-63 (48px)

#define SCREEN_W 128
#define SCREEN_H 64
#define YELLOW_H 16
#define BLUE_Y   16
#define BLUE_H   48

class BameGFX {
public:
  BameGFX(Adafruit_SSD1306 &disp);

  // Jauge dans la zone jaune avec texte XOR (font builtin size 2)
  // percent: 0-100, resolution = 126 colonnes
  // label: si non-null, affiche ce texte au lieu du %
  void drawGauge(float percent, const __FlashStringHelper *label = nullptr);

  // Titre dans la zone jaune (texte size 1, centre)
  void drawTitle(const __FlashStringHelper *title);

  // Texte dans la zone bleue (centre)
  // row: 0-4 (espacement 9px), size: 1 ou 2
  void drawText(uint8_t row, const char *text, uint8_t size = 1);
  void drawText(uint8_t row, const __FlashStringHelper *text, uint8_t size = 1);

  // Menu item avec prefixe de type et valeur optionnelle
  // prefix: '>' sous-menu, '.' valeur/action
  // selected: inverse video, editing: brackets + point clignotant
  void drawMenuItem(uint8_t row, char prefix, const __FlashStringHelper* label,
                    const char* value = nullptr, bool selected = false, bool editing = false);

  // Spinner calibration (rotatif 5px)
  void drawCalSpinner(int16_t x, int16_t y);

  // Barre de progression dans la zone bleue
  void drawProgress(uint8_t row, int percent);

  // Fleches animees decharge (>>>)
  void drawFlowArrows(int16_t x, int16_t y, uint8_t h = 8);

  // Icone batterie qui se remplit (animation charge)
  void drawChargingBattery(int16_t x, int16_t y);

  // Appeler dans loop() pour avancer l'animation
  void tick();

  // Dessine un nombre + "%" en XOR (font builtin, centre)
  void drawPercentXOR(int percent, int16_t y, int16_t zone_x = 0, int16_t zone_w = SCREEN_W);

  // Dessine un nombre en XOR (font builtin, centre, sans %)
  void drawNumberXOR(int number, int16_t y, int16_t zone_x = 0, int16_t zone_w = SCREEN_W);

private:
  Adafruit_SSD1306 &_disp;
  uint8_t _frame;
};
