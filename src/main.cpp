#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <avr/sleep.h>
#include <avr/power.h>
#include <avr/wdt.h>
#include <Adafruit_SSD1306.h>
#include <INA226.h>
#include "BameGFX.h"

// --- Configuration ---
#define BAME_VERSION "1.0"

#ifndef BAME_DEBUG
  #define BAME_DEBUG 0
#endif

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C

// Zones ecran bicolor definies dans BameGFX.h
#define INA226_ADDR 0x40

// Keypad Foxeer Key23 sur pin analogique
#define KEY_PIN A3
// Bouton action (NO, pull to GND)
#define ACTION_BTN_PIN 2

// Batterie LiFePO4 4S 80Ah
#define BATTERY_CAPACITY_AH 80.0
#define SHUNT_RESISTANCE 0.0025
#define MAX_CURRENT 30.0

// Seuils tension LFP 4S (valeurs usine, ecrasees par EEPROM)
#define VBAT_FULL_DEFAULT   14.6  // 4 x 3.65V
#define VBAT_EMPTY_DEFAULT  10.0  // 4 x 2.50V

// Seuils de detection
#define VBAT_REST_CURRENT  0.3    // courant max pour considerer "repos"
#define VBAT_CONVERGE_FAST 0.01   // convergence rapide (premiere calibration)
#define VBAT_CONVERGE_SLOW 0.001  // convergence lente (ajustement continu)

// EEPROM layout pour voltage calibration (apres keypad: addr 11+)
#define EEPROM_VCAL_MAGIC_ADDR 12
#define EEPROM_VCAL_ADDR       13  // 2 x float = 8 octets (13-20)
#define EEPROM_VCAL_MAGIC_VAL  0xBB

// EEPROM layout pour capacite apprise (apres vcal: addr 21+)
#define EEPROM_CAP_MAGIC_ADDR 21
#define EEPROM_CAP_ADDR       22  // 1 x float = 4 octets (22-25)
#define EEPROM_CAP_MAGIC_VAL  0xCC

// EEPROM layout pour capacite nominale (addr 26+)
#define EEPROM_NOM_MAGIC_ADDR 26
#define EEPROM_NOM_ADDR       27  // 1 x float = 4 octets (27-30)
#define EEPROM_NOM_MAGIC_VAL  0xDD

// addr 31-35 : libre

// EEPROM layout pour auto deep sleep
#define EEPROM_ASLEEP_ADDR        36  // 1 octet : 0x01 = actif

// Tensions dynamiques
float vbatMax = VBAT_FULL_DEFAULT;
float vbatMin = VBAT_EMPTY_DEFAULT;
float lastRestVoltage = 0;

// Capacite batterie
float batteryCapacityNom = BATTERY_CAPACITY_AH;  // nominale (user)
float batteryCapacityAh = BATTERY_CAPACITY_AH;   // estimee (calibration ou fallback nom)
float batteryCapacityAs = BATTERY_CAPACITY_AH * 3600.0;
bool capacityKnown = false;   // true si calibration fiable

// Calibration capacite par doublement exponentiel
float calCoulombs = 0;          // coulombs accumules segment en cours
float calTarget = 0;            // objectif coulombs (0 = mode temps initial)
float calStartVoltage = 0;      // tension repos debut segment
unsigned long calStartMs = 0;   // timestamp debut segment
#define CAL_INITIAL_TIME_MS  60000   // 1 min pour premier palier
#define CAL_MIN_COULOMBS     500.0   // ~0.14Ah minimum (precision INA226)

bool autoDeepSleep = false;   // deep sleep auto apres timeout inactivite

// --- Objets ---
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
BameGFX gfx(display);
INA226 ina(INA226_ADDR, &Wire);

// --- Variables globales ---
bool demoMode = false;
float voltage = 0;
float current = 0;
float power = 0;
float socPercent = 100.0;
float coulombCount = 0;
unsigned long lastMeasure = 0;
unsigned long lastDisplay = 0;

#define MEASURE_INTERVAL_MS 100
#define DISPLAY_INTERVAL_MS 500

// ===========================================
// Boutons - Foxeer Key23
// ===========================================
enum Button { BTN_NONE, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_CENTER };

// Forward declarations
void waitButtonRelease();
Button readButton();
void updateVoltageCalibration();
float socFromVoltage(float v);
void settingsMenu();
void batteryMenu();

// Veille
#define DEEPSLEEP_PRESS_MS 3000 // appui long -> veille profonde
#define AUTO_SLEEP_MS    60000   // 60s sans interaction -> veille ecran
#define AUTO_DEEPSLEEP_MS 300000 // 5min sans interaction -> deep sleep
bool oledSleeping = false;
unsigned long lastInteraction = 0;

// ISR pour reveil deep sleep (INT0 sur D2)
volatile bool wakeUpFlag = false;
void wakeUpISR() {
  wakeUpFlag = true;
}

void enterOledSleep() {
  oledSleeping = true;
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  #if BAME_DEBUG
  Serial.println(F("[SLEEP] OLED off"));
  #endif
}

void exitOledSleep() {
  oledSleeping = false;
  display.ssd1306_command(SSD1306_DISPLAYON);
  #if BAME_DEBUG
  Serial.println(F("[SLEEP] OLED on"));
  #endif
}

// ISR watchdog pour reveil periodique
volatile bool wdtWakeUp = false;
ISR(WDT_vect) {
  wdtWakeUp = true;
}

void enterDeepSleep() {
  #if BAME_DEBUG
  Serial.println(F("[SLEEP] Deep sleep..."));
  Serial.flush();
  #endif
  display.ssd1306_command(SSD1306_DISPLAYOFF);

  // Reveil par bouton D2
  attachInterrupt(digitalPinToInterrupt(ACTION_BTN_PIN), wakeUpISR, LOW);

  // Configurer watchdog 8s pour reveil periodique (mesure tension)
  cli();
  wdt_reset();
  MCUSR &= ~(1 << WDRF);
  WDTCSR |= (1 << WDCE) | (1 << WDE);
  WDTCSR = (1 << WDIE) | (1 << WDP3) | (1 << WDP0); // 8s, interrupt mode
  sei();

  // Intervalle adaptatif : 5min (38 cycles) a 1h (450 cycles)
  // Plus la tension baisse vite, plus on mesure souvent
  #define WDT_CYCLES_MIN  38   // 5 min
  #define WDT_CYCLES_MAX  450  // 1 heure
  uint16_t wdtTarget = WDT_CYCLES_MAX; // commence lent
  uint16_t wdtCount = 0;
  float prevVoltage = voltage;

  while (!wakeUpFlag) {
    wdtWakeUp = false;
    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    sleep_enable();
    sleep_mode();
    sleep_disable();

    if (wdtWakeUp && !wakeUpFlag) {
      wdtCount++;
      if (wdtCount >= wdtTarget) {
        wdtCount = 0;

        // Mesurer
        Wire.begin();
        if (ina.begin()) {
          ina.setMaxCurrentShunt(MAX_CURRENT, SHUNT_RESISTANCE);
          voltage = ina.getBusVoltage();
          current = ina.getCurrent();
          updateVoltageCalibration();

          // Adapter l'intervalle selon la variation de tension
          float deltaV = abs(voltage - prevVoltage);
          prevVoltage = voltage;

          if (deltaV > 0.5) {
            wdtTarget = WDT_CYCLES_MIN;        // chute rapide -> 5min
          } else if (deltaV > 0.1) {
            wdtTarget = WDT_CYCLES_MIN * 3;    // ~15min
          } else if (deltaV > 0.02) {
            wdtTarget = WDT_CYCLES_MAX / 2;    // ~30min
          } else {
            wdtTarget = WDT_CYCLES_MAX;         // stable -> 1h
          }
        }
      }
    }
  }

  // --- Reveil par bouton ---
  // Desactiver watchdog
  wdt_disable();
  detachInterrupt(digitalPinToInterrupt(ACTION_BTN_PIN));

  display.ssd1306_command(SSD1306_DISPLAYON);
  oledSleeping = false;
  wakeUpFlag = false;

  // Recaler SOC depuis tension au reveil
  socPercent = socFromVoltage(voltage);
  coulombCount = (socPercent / 100.0) * batteryCapacityAs;

  #if BAME_DEBUG
  Serial.println(F("[SLEEP] Wake up"));
  #endif
  waitButtonRelease();
}

// EEPROM layout pour la calibration keypad
#define EEPROM_MAGIC_ADDR 0        // 1 octet : 0xCA = calibre
#define EEPROM_CAL_ADDR   1        // 5 x 2 octets (int) = 10 octets
#define EEPROM_MAGIC_VAL  0xCA
#define CAL_BTN_COUNT 5

// Ordre : CENTER, UP, DOWN, LEFT, RIGHT (tries: 1, 416, 616, 748, 838)
// Ecarts: 1-416=415, 416-616=200, 616-748=132, 748-838=90, 838-1023=185
// Tolerance = min(ecart gauche, ecart droite) / 2
int keyCalVals[CAL_BTN_COUNT] = {838, 616, 1, 748, 416}; // defauts avec 10k pullup

bool keyCalibrated = false;

void saveCalToEEPROM() {
  EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VAL);
  for (int i = 0; i < CAL_BTN_COUNT; i++) {
    EEPROM.put(EEPROM_CAL_ADDR + i * 2, keyCalVals[i]);
  }
}

bool loadCalFromEEPROM() {
  if (EEPROM.read(EEPROM_MAGIC_ADDR) != EEPROM_MAGIC_VAL) return false;
  for (int i = 0; i < CAL_BTN_COUNT; i++) {
    EEPROM.get(EEPROM_CAL_ADDR + i * 2, keyCalVals[i]);
  }
  return true;
}

// --- Calibration voltage EEPROM ---
void saveVcalToEEPROM() {
  EEPROM.write(EEPROM_VCAL_MAGIC_ADDR, EEPROM_VCAL_MAGIC_VAL);
  EEPROM.put(EEPROM_VCAL_ADDR, vbatMax);
  EEPROM.put(EEPROM_VCAL_ADDR + sizeof(float), vbatMin);
}

bool loadVcalFromEEPROM() {
  if (EEPROM.read(EEPROM_VCAL_MAGIC_ADDR) != EEPROM_VCAL_MAGIC_VAL) return false;
  EEPROM.get(EEPROM_VCAL_ADDR, vbatMax);
  EEPROM.get(EEPROM_VCAL_ADDR + sizeof(float), vbatMin);
  // Sanity check
  if (vbatMax < 10.0 || vbatMax > 20.0) vbatMax = VBAT_FULL_DEFAULT;
  if (vbatMin < 5.0 || vbatMin > 15.0) vbatMin = VBAT_EMPTY_DEFAULT;
  if (vbatMin >= vbatMax) { vbatMax = VBAT_FULL_DEFAULT; vbatMin = VBAT_EMPTY_DEFAULT; }
  return true;
}

// --- EEPROM helpers pour float ---
void saveFloatEEPROM(uint8_t magicAddr, uint8_t magicVal, uint8_t dataAddr, float val) {
  EEPROM.write(magicAddr, magicVal);
  EEPROM.put(dataAddr, val);
}

bool loadFloatEEPROM(uint8_t magicAddr, uint8_t magicVal, uint8_t dataAddr, float &val) {
  if (EEPROM.read(magicAddr) != magicVal) return false;
  EEPROM.get(dataAddr, val);
  return true;
}

// Capacite estimee (calibration)
#define saveCapToEEPROM() saveFloatEEPROM(EEPROM_CAP_MAGIC_ADDR, EEPROM_CAP_MAGIC_VAL, EEPROM_CAP_ADDR, batteryCapacityAh)
bool loadCapFromEEPROM() {
  if (!loadFloatEEPROM(EEPROM_CAP_MAGIC_ADDR, EEPROM_CAP_MAGIC_VAL, EEPROM_CAP_ADDR, batteryCapacityAh)) return false;
  if (batteryCapacityAh < 1.0 || batteryCapacityAh > 500.0) batteryCapacityAh = batteryCapacityNom;
  batteryCapacityAs = batteryCapacityAh * 3600.0;
  capacityKnown = true;
  return true;
}

// Capacite nominale (user)
#define saveNomToEEPROM() saveFloatEEPROM(EEPROM_NOM_MAGIC_ADDR, EEPROM_NOM_MAGIC_VAL, EEPROM_NOM_ADDR, batteryCapacityNom)
bool loadNomFromEEPROM() {
  if (!loadFloatEEPROM(EEPROM_NOM_MAGIC_ADDR, EEPROM_NOM_MAGIC_VAL, EEPROM_NOM_ADDR, batteryCapacityNom)) return false;
  if (batteryCapacityNom < 1.0 || batteryCapacityNom > 500.0) batteryCapacityNom = BATTERY_CAPACITY_AH;
  return true;
}

void resetCapEEPROM() {
  EEPROM.write(EEPROM_CAP_MAGIC_ADDR, 0xFF);
  capacityKnown = false;
  batteryCapacityAh = batteryCapacityNom;  // fallback sur nominal
  batteryCapacityAs = batteryCapacityNom * 3600.0;
  calCoulombs = 0;
  calTarget = 0;
  calStartVoltage = 0;
  calStartMs = 0;
}

void saveAutoSleepToEEPROM() { EEPROM.write(EEPROM_ASLEEP_ADDR, autoDeepSleep ? 0x01 : 0x00); }
void loadAutoSleepFromEEPROM() { autoDeepSleep = (EEPROM.read(EEPROM_ASLEEP_ADDR) == 0x01); }

// Mise a jour autocal tension
// Conditions : repos (|courant| < seuil) ET pas en charge (courant >= 0)
unsigned long lastVcalSave = 0;
#define VCAL_SAVE_INTERVAL 60000  // sauver EEPROM max toutes les 60s

void updateVoltageCalibration() {
  // Ignorer si en charge (tension artificiellement haute)
  if (current < -VBAT_REST_CURRENT) return;
  // Ignorer si pas au repos
  if (abs(current) > VBAT_REST_CURRENT) return;

  lastRestVoltage = voltage;

  // Vitesse de convergence : rapide si loin, lente si proche
  float conv;

  // Mise a jour Vmax
  if (voltage > vbatMax * 0.98) {
    conv = (voltage > vbatMax) ? VBAT_CONVERGE_FAST : VBAT_CONVERGE_SLOW;
    vbatMax = vbatMax * (1.0 - conv) + voltage * conv;
  }

  // Mise a jour Vmin
  if (voltage < vbatMin * 1.05) {
    conv = (voltage < vbatMin) ? VBAT_CONVERGE_FAST : VBAT_CONVERGE_SLOW;
    vbatMin = vbatMin * (1.0 - conv) + voltage * conv;
  }

  // Sauvegarder periodiquement en EEPROM (pas a chaque mesure)
  if (millis() - lastVcalSave >= VCAL_SAVE_INTERVAL) {
    saveVcalToEEPROM();
    lastVcalSave = millis();
  }
}

// Seuils calcules dynamiquement a partir des valeurs calibrees
// Chaque bouton a sa propre tolerance = distance au voisin le plus proche / 2
int keyThresholds[CAL_BTN_COUNT];

void computeThresholds() {
  // Trier les valeurs pour trouver les distances
  int sorted[CAL_BTN_COUNT + 1]; // +1 pour NONE (1023)
  int sortIdx[CAL_BTN_COUNT];
  for (int i = 0; i < CAL_BTN_COUNT; i++) {
    sorted[i] = keyCalVals[i];
    sortIdx[i] = i;
  }
  sorted[CAL_BTN_COUNT] = 1023; // valeur repos

  // Tri simple des valeurs
  for (int i = 0; i < CAL_BTN_COUNT; i++) {
    for (int j = i + 1; j < CAL_BTN_COUNT; j++) {
      if (sorted[j] < sorted[i]) {
        int tmp = sorted[i]; sorted[i] = sorted[j]; sorted[j] = tmp;
        tmp = sortIdx[i]; sortIdx[i] = sortIdx[j]; sortIdx[j] = tmp;
      }
    }
  }

  // Calculer le seuil pour chaque bouton
  for (int i = 0; i < CAL_BTN_COUNT; i++) {
    int distLeft = (i == 0) ? sorted[i] : sorted[i] - sorted[i - 1];
    int distRight = sorted[i + 1] - sorted[i];
    int minDist = (distLeft < distRight) ? distLeft : distRight;
    keyThresholds[sortIdx[i]] = minDist / 2 - 1;
    if (keyThresholds[sortIdx[i]] < 10) keyThresholds[sortIdx[i]] = 10;
  }
}

Button readButton() {
  // Bouton action physique = CENTER
  if (digitalRead(ACTION_BTN_PIN) == LOW) return BTN_CENTER;

  // Pad Foxeer
  int val = analogRead(KEY_PIN);
  if (abs(val - keyCalVals[0]) < keyThresholds[0]) return BTN_CENTER;
  if (abs(val - keyCalVals[1]) < keyThresholds[1]) return BTN_UP;
  if (abs(val - keyCalVals[2]) < keyThresholds[2]) return BTN_DOWN;
  if (abs(val - keyCalVals[3]) < keyThresholds[3]) return BTN_LEFT;
  if (abs(val - keyCalVals[4]) < keyThresholds[4]) return BTN_RIGHT;
  return BTN_NONE;
}

// Anti-rebond : retourne le bouton si stable pendant 50ms
Button readButtonDebounced() {
  Button b = readButton();
  if (b == BTN_NONE) return BTN_NONE;
  delay(50);
  Button b2 = readButton();
  return (b == b2) ? b : BTN_NONE;
}

// Attend que le bouton soit relache
void waitButtonRelease() {
  while (readButton() != BTN_NONE) delay(10);
}

// Appui long -> deep sleep, utilisable depuis n'importe quel ecran
// Appeler quand BTN_CENTER est detecte. Retourne true si deep sleep declenche.
bool checkLongPress() {
  unsigned long pressStart = millis();

  // Deep sleep toujours disponible (calibration s'arrete en deep sleep)

  while (readButton() == BTN_CENTER) {
    unsigned long held = millis() - pressStart;

    if (held > 500) {
      float pct = (float)held * 100.0f / DEEPSLEEP_PRESS_MS;
      if (pct > 100.0f) pct = 100.0f;
      display.clearDisplay();
      gfx.drawGauge(pct, F("SLEEP"));
      display.display();
    }

    if (held >= DEEPSLEEP_PRESS_MS) {
      waitButtonRelease();
      enterDeepSleep();
      lastMeasure = millis();
      lastInteraction = millis();
      return true;
    }

    delay(50);
  }
  return false;
}

// ===========================================
// SOC
// ===========================================
struct SocPoint {
  float voltage;
  float soc;
};

const SocPoint socCurve[] = {
  {14.6, 100}, {13.6,  99}, {13.4,  90}, {13.3,  70},
  {13.2,  40}, {13.1,  30}, {13.0,  20}, {12.8,  17},
  {12.0,  14}, {11.0,   9}, {10.0,   0},
};
const int socCurveSize = sizeof(socCurve) / sizeof(socCurve[0]);

float socFromVoltage(float v) {
  // Rescale la tension selon les bornes dynamiques
  // La courbe est definie pour VBAT_EMPTY_DEFAULT..VBAT_FULL_DEFAULT
  // On mappe v dans cet espace selon vbatMin..vbatMax
  float vScaled = VBAT_EMPTY_DEFAULT +
    (v - vbatMin) / (vbatMax - vbatMin) *
    (VBAT_FULL_DEFAULT - VBAT_EMPTY_DEFAULT);

  if (vScaled >= socCurve[0].voltage) return 100.0;
  if (vScaled <= socCurve[socCurveSize - 1].voltage) return 0.0;
  for (int i = 0; i < socCurveSize - 1; i++) {
    if (vScaled >= socCurve[i + 1].voltage) {
      float ratio = (vScaled - socCurve[i + 1].voltage) / (socCurve[i].voltage - socCurve[i + 1].voltage);
      return socCurve[i + 1].soc + ratio * (socCurve[i].soc - socCurve[i + 1].soc);
    }
  }
  return 0.0;
}

// ===========================================
// Diagnostics
// ===========================================

// --- Diag 1 : Calibration keypad ---
void diagKeypad() {
  #if BAME_DEBUG
  Serial.println(F("[DIAG] Calibration keypad"));
  Serial.println(F("Appuie sur chaque bouton, note les valeurs ADC"));
  Serial.println(F("Centre pour quitter"));
  #endif

  #if BAME_DEBUG
  int lastVal = -1;
  #endif
  bool quit = false;

  while (!quit) {
    int val = analogRead(KEY_PIN);

    // Affichage OLED
    display.clearDisplay();
    // Zone jaune : titre
    display.setTextSize(1);
    display.setCursor(0, 4);
    display.print(F("DIAG: Keypad cal"));

    // Zone bleue : contenu
    display.setTextSize(2);
    display.setCursor(0, BLUE_Y + 2);
    display.print(F("ADC:"));
    display.println(val);

    display.setTextSize(1);
    display.setCursor(0, BLUE_Y + 22);
    Button b = readButton();
    display.print(F("Button: "));
    switch (b) {
      case BTN_UP:     display.print(F("UP"));      break;
      case BTN_DOWN:   display.print(F("DOWN"));    break;
      case BTN_LEFT:   display.print(F("LEFT"));    break;
      case BTN_RIGHT:  display.print(F("RIGHT"));   break;
      case BTN_CENTER: display.print(F("CENTER"));  break;
      default:         display.print(F("-"));        break;
    }

    display.display();

    // Serie : afficher seulement quand la valeur change significativement
    #if BAME_DEBUG
    if (abs(val - lastVal) > 5) {
      Serial.print(F("ADC: "));
      Serial.println(val);
      lastVal = val;
    }
    #endif

    // Quitter avec gauche maintenu 1s
    if (b == BTN_LEFT) {
      delay(500);
      if (readButton() == BTN_LEFT) quit = true;
    }

    delay(100);
  }
  waitButtonRelease();
}

// --- Diag 2 : Scan I2C ---
void diagI2CScan() {
  display.clearDisplay();
  gfx.drawTitle(F("I2C SCAN"));
  display.setTextSize(1);
  display.setCursor(0, BLUE_Y + 2);
  int found = 0;
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      display.print(F("0x"));
      if (addr < 16) display.print(F("0"));
      display.print(addr, HEX);
      display.print(F(" "));
      found++;
    }
  }
  display.setCursor(0, BLUE_Y + 22);
  display.print(found);
  display.print(F(" device(s)"));
  display.display();

  while (true) {
    Button b = readButtonDebounced();
    if (b == BTN_LEFT) break;
    if (b == BTN_CENTER && checkLongPress()) return;
    delay(50);
  }
  waitButtonRelease();
}

// --- Diag 4 : Test INA226 ---
void diagINA226() {
  bool ok = ina.begin();
  if (!ok) {
    display.clearDisplay();
    gfx.drawTitle(F("INA226"));
    gfx.drawText(1, F("NOT FOUND"), 2);
    display.display();
    delay(2000);
    return;
  }

  ina.setMaxCurrentShunt(MAX_CURRENT, SHUNT_RESISTANCE);
  ina.setAverage(4);

  while (true) {
    float v = ina.getBusVoltage();
    float a = ina.getCurrent();
    float w = ina.getPower();

    display.clearDisplay();
    gfx.drawTitle(F("INA226"));
    display.setTextSize(1);
    display.setCursor(0, BLUE_Y + 2);
    display.print(F("V: ")); display.print(v, 3); display.println(F(" V"));
    display.print(F("A: ")); display.print(a, 3); display.println(F(" A"));
    display.print(F("W: ")); display.print(w, 3); display.print(F(" W"));
    display.display();

    Button b = readButtonDebounced();
    if (b == BTN_LEFT) break;
    if (b == BTN_CENTER && checkLongPress()) return;
    delay(200);
  }
  waitButtonRelease();
}

// --- Diag 5 : Infos systeme ---
extern int __heap_start, *__brkval;
int freeRAM() {
  int v;
  return (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
}

void diagSysInfo() {
  display.clearDisplay();
  gfx.drawTitle(F("SYSTEM"));
  display.setTextSize(1);
  display.setCursor(0, BLUE_Y + 2);
  display.print(F("BAME v" BAME_VERSION));
  display.println();
  display.print(F("Clock: "));
  display.print(F_CPU / 1000000UL);
  display.println(F(" MHz"));
  display.print(F("RAM: "));
  display.print(freeRAM());
  display.println(F(" free"));
  display.print(F("Uptime: "));
  display.print(millis() / 1000);
  display.print(F("s"));
  display.display();

  while (true) {
    Button b = readButtonDebounced();
    if (b == BTN_LEFT) break;
    if (b == BTN_CENTER && checkLongPress()) return;
    delay(50);
  }
  waitButtonRelease();
}

// --- Diag 3 : Test ecran (tous pixels allumes) ---
void diagScreen() {
  display.clearDisplay();
  display.fillRect(0, 0, SCREEN_W, SCREEN_H, SSD1306_WHITE);
  display.display();

  while (true) {
    Button b = readButtonDebounced();
    if (b == BTN_LEFT) break;
    if (b == BTN_CENTER && checkLongPress()) return;
    delay(50);
  }
  waitButtonRelease();
}

// --- Batterie : page info (lecture seule) ---
void batteryInfo() {
  while (true) {
    display.clearDisplay();
    gfx.drawTitle(F("BAT INFO"));
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    // Ligne 1 : Capacite estimee [nominale]
    display.setCursor(0, BLUE_Y);
    display.print(F("Cap:"));
    if (capacityKnown) {
      display.print((int)batteryCapacityAh);
    } else {
      display.print(F("--"));
    }
    display.print(F("Ah ["));
    display.print((int)batteryCapacityNom);
    display.print(F("]"));

    // Ligne 2 : Calibration + delta%
    display.setCursor(0, BLUE_Y + 9);
    float calAh = calCoulombs / 3600.0;
    display.print(calAh, 1);
    display.print(F("/"));
    if (calTarget > 0) display.print(calTarget / 3600.0, 0);
    else display.print('-');
    display.print(F("Ah"));
    if (calStartVoltage > 0 && lastRestVoltage > 0) {
      int ds = (int)(socFromVoltage(calStartVoltage) - socFromVoltage(lastRestVoltage));
      if (ds > 0) { display.print(F(" (")); display.print(ds); display.print(F("%)")); }
    }

    // Ligne 3 : Segment V
    display.setCursor(0, BLUE_Y + 18);
    if (calStartVoltage > 0) display.print(calStartVoltage, 2);
    else display.print('-');
    display.print(F(">"));
    if (lastRestVoltage > 0) display.print(lastRestVoltage, 2);
    else display.print('-');
    display.print('V');

    // Ligne 4 : Vmax/Vmin
    display.setCursor(0, BLUE_Y + 27);
    display.print(vbatMax, 2);
    display.print('/');
    display.print(vbatMin, 2);
    display.print('V');

    display.display();

    Button b = readButtonDebounced();
    if (b == BTN_LEFT || b == BTN_CENTER) break;
    delay(100);
  }
  waitButtonRelease();
}

// --- Batterie : parametres batterie ---
void batteryMenu() {
  uint8_t sel = 0;
  const uint8_t count = 4;
  int8_t editing = -1;

  float tmpCap = batteryCapacityNom;
  bool tmpReset = false;
  bool tmpSleep = autoDeepSleep;

  while (true) {
    display.clearDisplay();
    gfx.drawTitle(F("BATTERY"));

    char buf[10];

    // Capacite nominale
    int capVal = (int)(editing == 0 ? tmpCap : batteryCapacityNom);
    itoa(capVal, buf, 10);
    strcat(buf, "Ah");
    gfx.drawMenuItem(0, ' ', F("Capacity"), buf, sel == 0, editing == 0);

    // Reset cal
    gfx.drawMenuItem(1, ' ', F("Reset cal"),
      editing == 1 ? (tmpReset ? "YES" : "NO") : "",
      sel == 1, editing == 1);

    // Auto sleep
    gfx.drawMenuItem(2, ' ', F("Sleep"),
      (editing == 2 ? tmpSleep : autoDeepSleep) ? "ON" : "OFF",
      sel == 2, editing == 2);

    // Info
    gfx.drawMenuItem(3, '>', F("Info"), NULL, sel == 3);

    display.display();

    Button b = readButtonDebounced();
    if (b == BTN_NONE) { delay(50); continue; }

    if (editing >= 0) {
      switch (b) {
        case BTN_UP:
          if (editing == 0) { tmpCap += 1; if (tmpCap > 500) tmpCap = 500; }
          else if (editing == 1) { tmpReset = !tmpReset; }
          else if (editing == 2) { tmpSleep = !tmpSleep; }
          break;
        case BTN_DOWN:
          if (editing == 0) { tmpCap -= 1; if (tmpCap < 1) tmpCap = 1; }
          else if (editing == 1) { tmpReset = !tmpReset; }
          else if (editing == 2) { tmpSleep = !tmpSleep; }
          break;
        case BTN_CENTER:
          if (editing == 0) {
            batteryCapacityNom = tmpCap;
            saveNomToEEPROM();
            // Si pas de calibration, fallback sur nominal
            if (!capacityKnown) {
              batteryCapacityAh = batteryCapacityNom;
              batteryCapacityAs = batteryCapacityNom * 3600.0;
            }
          } else if (editing == 1) {
            if (tmpReset) { resetCapEEPROM(); tmpReset = false; }
          } else if (editing == 2) {
            autoDeepSleep = tmpSleep;
            saveAutoSleepToEEPROM();
          }
          editing = -1;
          break;
        case BTN_LEFT:
          tmpCap = batteryCapacityNom;
          tmpReset = false;
          tmpSleep = autoDeepSleep;
          editing = -1;
          break;
        default: break;
      }
    } else {
      switch (b) {
        case BTN_UP: sel = (sel == 0) ? count - 1 : sel - 1; break;
        case BTN_DOWN: sel = (sel + 1) % count; break;
        case BTN_CENTER:
          if (sel == 3) {
            waitButtonRelease();
            batteryInfo();
          } else {
            editing = sel;
            tmpCap = batteryCapacityNom;
            tmpReset = false;
            tmpSleep = autoDeepSleep;
          }
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

// --- Menu principal SYSTEM (flat, pagine) ---
#define SYS_MENU_COUNT 7
#define SYS_PAGE_SIZE  5

void settingsMenu() {
  uint8_t sel = 0;
  int8_t editing = -1;
  bool tmpDemo = false;

  while (true) {
    uint8_t page = sel / SYS_PAGE_SIZE;
    uint8_t pages = (SYS_MENU_COUNT + SYS_PAGE_SIZE - 1) / SYS_PAGE_SIZE;
    uint8_t pageStart = page * SYS_PAGE_SIZE;
    uint8_t pageEnd = pageStart + SYS_PAGE_SIZE;
    if (pageEnd > SYS_MENU_COUNT) pageEnd = SYS_MENU_COUNT;

    display.clearDisplay();

    // Titre avec pagination
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    if (pages > 1) {
      char title[12];
      strcpy(title, "SYSTEM ");
      title[7] = '1' + page;
      title[8] = '/';
      title[9] = '0' + pages;
      title[10] = 0;
      int16_t tw = strlen(title) * 6;
      display.setCursor((SCREEN_WIDTH - tw) / 2, 4);
      display.print(title);
    } else {
      gfx.drawTitle(F("SYSTEM"));
    }

    // Items visibles sur cette page
    for (uint8_t i = pageStart; i < pageEnd; i++) {
      uint8_t row = i - pageStart;
      bool s = (i == sel);
      switch (i) {
        case 0: gfx.drawMenuItem(row, '>', F("Battery"), NULL, s); break;
        case 1: gfx.drawMenuItem(row, '>', F("Keypad"), NULL, s); break;
        case 2: gfx.drawMenuItem(row, '>', F("I2C"), NULL, s); break;
        case 3: gfx.drawMenuItem(row, '>', F("Screen"), NULL, s); break;
        case 4: gfx.drawMenuItem(row, '>', F("INA226"), NULL, s); break;
        case 5: gfx.drawMenuItem(row, '>', F("Sys info"), NULL, s); break;
        case 6: gfx.drawMenuItem(row, ' ', F("Demo"),
          editing == 6 ? (tmpDemo ? "YES" : "NO") : "",
          s, editing == 6); break;
      }
    }

    display.display();

    Button b = readButtonDebounced();
    if (b == BTN_NONE) { delay(50); continue; }

    if (editing >= 0) {
      switch (b) {
        case BTN_UP:
        case BTN_DOWN:
          tmpDemo = !tmpDemo;
          break;
        case BTN_CENTER:
          if (tmpDemo) { demoMode = true; return; }
          editing = -1;
          break;
        case BTN_LEFT:
          tmpDemo = false;
          editing = -1;
          break;
        default: break;
      }
    } else {
      switch (b) {
        case BTN_UP:
          sel = (sel == 0) ? SYS_MENU_COUNT - 1 : sel - 1;
          break;
        case BTN_DOWN:
          sel = (sel + 1) % SYS_MENU_COUNT;
          break;
        case BTN_CENTER:
          waitButtonRelease();
          if (sel == 6) {
            editing = 6;
            tmpDemo = false;
          } else {
            switch (sel) {
              case 0: batteryMenu(); break;
              case 1: diagKeypad(); break;
              case 2: diagI2CScan(); break;
              case 3: diagScreen(); break;
              case 4: diagINA226(); break;
              case 5: diagSysInfo(); break;
            }
          }
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

// ===========================================
// Mode normal
// ===========================================

// --- Lectures capteur (simulees en demoMode) ---
float simSoc = 100.0;
bool simDirection = false; // false = descend, true = monte

float readVoltage() {
  if (demoMode) {
    return 10.0 + (simSoc / 100.0) * 4.6;
  }
  return ina.getBusVoltage();
}

float readCurrent() {
  if (demoMode) {
    return simDirection ? -5.0 : 12.5;
  }
  return ina.getCurrent();
}

float readPower() {
  if (demoMode) {
    return readVoltage() * abs(readCurrent());
  }
  return ina.getPower();
}

void updateSimSoc() {
  if (!demoMode) return;
  if (simDirection) {
    simSoc += 0.05;
    if (simSoc >= 100.0) { simSoc = 100.0; simDirection = false; }
  } else {
    simSoc -= 0.05;
    if (simSoc <= 0.0) { simSoc = 0.0; simDirection = true; }
  }
}

void updateMeasurements() {
  updateSimSoc();

  voltage = readVoltage();
  current = readCurrent();
  power = readPower();

  unsigned long now = millis();
  float dtSeconds = (now - lastMeasure) / 1000.0;
  lastMeasure = now;

  if (demoMode) {
    socPercent = simSoc;
    coulombCount = (socPercent / 100.0) * batteryCapacityAs;
    return;
  }

  // Pas de batterie
  static bool batteryPresent = false;
  if (voltage < 1.0) {
    batteryPresent = false;
    socPercent = 0;
    coulombCount = 0;
    return;
  }

  // 1) Init : attendre tension stabilisee avant de verrouiller SOC
  if (!batteryPresent) {
    socPercent = socFromVoltage(voltage);
    coulombCount = (socPercent / 100.0) * batteryCapacityAs;
    if (voltage >= vbatMin) {
      batteryPresent = true;
    }
    return;
  }

  // 2) Coulomb counting
  coulombCount -= current * dtSeconds;
  if (coulombCount > batteryCapacityAs) coulombCount = batteryCapacityAs;
  if (coulombCount < 0) coulombCount = 0;
  socPercent = (coulombCount / batteryCapacityAs) * 100.0;

  // 4) Calibration par doublement exponentiel
  //    Accumule coulombs en decharge, sauvegarde quand objectif atteint ET repos
  if (current > 0) {
    calCoulombs += current * dtSeconds;
  }

  // Verifier si on a atteint l'objectif de calibration
  bool calObjectifAtteint;
  if (calTarget <= 0) {
    // Mode temps initial : 1 min ET minimum coulombs
    calObjectifAtteint = (now - calStartMs >= CAL_INITIAL_TIME_MS)
                      && (calCoulombs >= CAL_MIN_COULOMBS);
  } else {
    // Mode doublement : objectif coulombs
    calObjectifAtteint = (calCoulombs >= calTarget);
  }

  // Sauvegarder quand objectif atteint ET au repos (tension fiable)
  if (calObjectifAtteint && abs(current) < VBAT_REST_CURRENT && calCoulombs > 0) {
    float vEnd = voltage;
    float socStart = socFromVoltage(calStartVoltage);
    float socEnd = socFromVoltage(vEnd);
    float deltaSoc = socStart - socEnd;

    // Estimer capacite si delta SOC suffisant
    if (deltaSoc > 5.0 && calStartVoltage > 0) {
      float estAh = (calCoulombs / 3600.0) / (deltaSoc / 100.0);
      // Sanity check
      if (estAh > 1.0 && estAh < 500.0) {
        if (!capacityKnown) {
          // Premiere estimation : accepter si delta > 30%
          if (deltaSoc > 30.0) {
            batteryCapacityAh = estAh;
            batteryCapacityAs = estAh * 3600.0;
            capacityKnown = true;
            saveCapToEEPROM();
            // Calibration fiable → activer deep sleep auto
            if (!autoDeepSleep) {
              autoDeepSleep = true;
              saveAutoSleepToEEPROM();
            }
          }
        } else {
          // Convergence ponderee par delta SOC
          float weight = constrain(deltaSoc / 100.0, 0.05, 0.5);
          batteryCapacityAh = batteryCapacityAh * (1.0 - weight) + estAh * weight;
          batteryCapacityAs = batteryCapacityAh * 3600.0;
          saveCapToEEPROM();
        }
      }
    }

    // Doubler l'objectif pour le palier suivant
    calTarget = calCoulombs * 2.0;
    // Nouveau segment : repos actuel = debut du suivant
    calStartVoltage = vEnd;
    calStartMs = now;
    calCoulombs = 0;
  }

  // 5) Au repos : recaler SOC vers tension (correction unique, pas blend continu)
  if (abs(current) < VBAT_REST_CURRENT) {
    float socV = socFromVoltage(voltage);
    socPercent = socPercent * 0.9 + socV * 0.1;
    coulombCount = (socPercent / 100.0) * batteryCapacityAs;
    // Memoriser tension repos pour calibration
    lastRestVoltage = voltage;
    // Initialiser debut segment si pas encore fait
    if (calStartVoltage == 0) {
      calStartVoltage = voltage;
      calStartMs = now;
    }
  }

  updateVoltageCalibration();
}

void updateDisplay() {
  display.clearDisplay();

  // Pas de batterie detectee
  if (!demoMode && voltage < 1.0) {
    gfx.drawGauge(0);
    display.setTextSize(2);
    display.setCursor(4, BLUE_Y + 12);
    if ((millis() / 500) % 2) display.print(F("No Battery"));
    display.display();
    return;
  }

  // === Zone JAUNE (0-15) : Jauge SOC avec sprites XOR ===
  gfx.drawGauge(socPercent);

  // === Zone BLEUE (16-63) ===

  // Ligne 1 : Tension + etat
  display.setTextSize(2);
  display.setCursor(0, BLUE_Y + 2);
  display.print(voltage, 1);
  display.setTextSize(1);
  display.print(F("V"));

  // Capacite restante en Ah, aligne a droite
  int ahInt = (int)(coulombCount / 3600.0);
  uint8_t ahDigits = 1;
  if (ahInt >= 10)   ahDigits = 2;
  if (ahInt >= 100)  ahDigits = 3;
  int16_t ahX = SCREEN_W - (ahDigits * 12 + 12);
  display.setTextSize(2);
  display.setCursor(ahX, BLUE_Y + 2);
  display.print(ahInt);
  display.setTextSize(1);
  display.print(F("Ah"));

  // Ligne 2 : Puissance + courant
  display.setTextSize(1);
  display.setCursor(0, BLUE_Y + 22);
  display.print((int)abs(power));
  display.print(F("W"));
  // Amperes aligne a droite
  {
    // Compter les chars de current avec 1 decimale + "A"
    // signe(0-1) + partie entiere(1-3) + '.' + decimale + 'A'
    int ci = (int)abs(current);
    uint8_t alen = 4; // "X.XA" minimum
    if (ci >= 10) alen++;
    if (ci >= 100) alen++;
    if (current < 0) alen++;
    display.setCursor(SCREEN_W - alen * 6, BLUE_Y + 22);
    display.print(current, 1);
    display.print(F("A"));
  }

  // Ligne 3 : Fleche + temps restant
  int16_t ty = BLUE_Y + 37;
  float hoursLeft = 0;
  if (current > 0.5) {
    hoursLeft = (coulombCount / 3600.0) / current;
  } else if (current < -0.5) {
    float remaining = (batteryCapacityAs - coulombCount) / 3600.0;
    if (remaining < 0) remaining = 0;
    hoursLeft = remaining / (-current);
  }

  if (abs(current) > 0.5) {
    if (hoursLeft < 0) hoursLeft = 0;
    if (hoursLeft > 99.9) hoursLeft = 99.9;
    int h = (int)hoursLeft;
    int m = (int)((hoursLeft - h) * 60);

    // Triangle : < decharge, > charge
    if (current > 0) display.fillTriangle(0, ty + 3, 6, ty, 6, ty + 6, SSD1306_WHITE);
    else             display.fillTriangle(6, ty + 3, 0, ty, 0, ty + 6, SSD1306_WHITE);

    display.setCursor(10, ty);
    char tbuf[6];
    tbuf[0] = '0' + (h / 10); tbuf[1] = '0' + (h % 10);
    tbuf[2] = ':';
    tbuf[3] = '0' + (m / 10); tbuf[4] = '0' + (m % 10);
    tbuf[5] = 0;
    display.print(tbuf);

    // Bas droite : cumul calibration (decharge) ou icone batterie (charge)
    if (current > 0 && !autoDeepSleep) {
      gfx.drawCalSpinner(SCREEN_W - 38, ty + 3);
      float calAh = calCoulombs / 3600.0;
      display.setCursor(SCREEN_W - 30, ty);
      if (calAh >= 10.0) display.print((int)calAh);
      else display.print(calAh, 1);
      display.print(F("Ah"));
    } else if (current < 0) {
      gfx.drawChargingBattery(106, ty);
    }
  }

  display.display();
}

#if BAME_DEBUG
void debugSerial() {
  Serial.print(voltage, 2);
  Serial.print(F(" V | "));
  Serial.print(current, 2);
  Serial.print(F(" A | "));
  Serial.print(power, 1);
  Serial.print(F(" W | SOC: "));
  Serial.print(socPercent, 1);
  Serial.print(F("% | Coulombs: "));
  Serial.print(coulombCount, 0);
  Serial.print(F("/"));
  Serial.print(batteryCapacityAs, 0);
  if (current > 0.5) {
    float hoursLeft = (coulombCount / 3600.0) / current;
    Serial.print(F(" | Reste: "));
    Serial.print(hoursLeft, 1);
    Serial.print(F("h"));
  }
  Serial.println();
}
#endif

// ===========================================
// Setup & Loop
// ===========================================

void setup() {
  #if BAME_DEBUG
  Serial.begin(115200);
  delay(500);
  #endif
  Wire.begin();
  pinMode(KEY_PIN, INPUT);
  pinMode(ACTION_BTN_PIN, INPUT_PULLUP);
  computeThresholds(); // seuils par defaut

  #if BAME_DEBUG
  Serial.println(F(""));
  Serial.println(F("========================="));
  Serial.println(F(" BAME Battery Monitor"));
  Serial.println(F(" LFP 4S 80Ah"));
  Serial.println(F("========================="));

  // Scan I2C
  Serial.println(F("[I2C] Scan..."));
  {
    int found = 0;
    for (byte addr = 1; addr < 127; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0) {
        Serial.print(F("  0x"));
        if (addr < 16) Serial.print(F("0"));
        Serial.println(addr, HEX);
        found++;
      }
    }
    Serial.print(found);
    Serial.println(F(" peripherique(s)"));
  }
  #endif

  // Init OLED
  #if BAME_DEBUG
  Serial.println(F("[OLED] Init..."));
  #endif
  bool oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  #if BAME_DEBUG
  if (!oledOk) {
    Serial.println(F("[OLED] ERREUR - non trouve"));
  } else {
    Serial.println(F("[OLED] OK"));
  }
  #endif
  if (oledOk) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
  }

  // Keypad : charger calibration EEPROM
  const char* btnNames[] = {"CENTER", "UP", "DOWN", "LEFT", "RIGHT"};
  loadCalFromEEPROM();
  computeThresholds();
  keyCalibrated = true;

  // Bouton maintenu au boot -> mode diag/calibration
  delay(200);
  if (analogRead(KEY_PIN) < 1000) {
    #define BOOT_CAL_MS 5000
    unsigned long pressStart = millis();
    bool calibrate = false;

    // Jauge 0->100% pendant 5s
    while (analogRead(KEY_PIN) < 1000) {
      unsigned long held = millis() - pressStart;
      float pct = (float)held * 100.0f / BOOT_CAL_MS;
      if (pct > 100.0f) pct = 100.0f;

      if (oledOk) {
        display.clearDisplay();
        gfx.drawGauge(pct, F("CAL"));
        gfx.drawText(1, F("Hold = calibrate"));
        display.display();
      }

      if (held >= BOOT_CAL_MS) {
        calibrate = true;
        break;
      }
      delay(50);
    }
    waitButtonRelease();

    if (calibrate) {
      // Calibration keypad
      delay(300);
      for (int b = 0; b < CAL_BTN_COUNT; b++) {
        int stableMin = 1023;
        bool pressed = false;
        bool done = false;

        while (!done) {
          int val = analogRead(KEY_PIN);
          if (val < 1000) {
            if (val < stableMin) stableMin = val;
            pressed = true;
          } else if (pressed) {
            keyCalVals[b] = stableMin;
            done = true;
          }

          if (oledOk) {
            display.clearDisplay();
            gfx.drawTitle(F("CALIBRATION"));
            display.setTextSize(2);
            display.setCursor(0, BLUE_Y + 2);
            display.println(btnNames[b]);
            display.setTextSize(1);
            display.setCursor(0, BLUE_Y + 22);
            display.print(F("ADC: "));
            display.print(val);
            if (pressed) {
              display.print(F("  min:"));
              display.print(stableMin);
            }
            display.display();
          }
          delay(50);
        }
        delay(300);
      }

      saveCalToEEPROM();
      computeThresholds();

      if (oledOk) {
        display.clearDisplay();
        gfx.drawTitle(F("CAL OK"));
        display.setTextSize(1);
        display.setCursor(0, BLUE_Y + 2);
        for (int i = 0; i < CAL_BTN_COUNT; i++) {
          display.print(btnNames[i]);
          display.print(F(": "));
          display.println(keyCalVals[i]);
        }
        display.display();
        delay(2000);
      }
    } else {
      // Appui court -> menu reglages
      settingsMenu();
    }
  }

  // Charger calibration tension depuis EEPROM
  if (loadVcalFromEEPROM()) {
    #if BAME_DEBUG
    Serial.println(F("[VCAL] EEPROM loaded"));
    Serial.print(F("  Vmax=")); Serial.print(vbatMax, 2);
    Serial.print(F("  Vmin=")); Serial.println(vbatMin, 2);
    #endif
  } else {
    #if BAME_DEBUG
    Serial.println(F("[VCAL] Using defaults"));
    #endif
  }

  // Charger capacite apprise + tension de charge
  loadNomFromEEPROM();  // nominal d'abord (fallback)
  if (!loadCapFromEEPROM()) {
    // Pas de calibration → fallback sur nominal
    batteryCapacityAh = batteryCapacityNom;
    batteryCapacityAs = batteryCapacityNom * 3600.0;
  }
  loadAutoSleepFromEEPROM();
  // calCoulombs repart a 0 a chaque boot (segment frais)

  // Init INA226
  bool inaOk = ina.begin();
  if (!inaOk) {
    #if BAME_DEBUG
    Serial.println(F("[INA226] ERREUR - non trouve"));
    #endif
  } else {
    #if BAME_DEBUG
    Serial.println(F("[INA226] OK"));
    #endif
    ina.setMaxCurrentShunt(MAX_CURRENT, SHUNT_RESISTANCE);
    ina.setAverage(4);
  }

  // SOC initial
  delay(500);
  if (inaOk) {
    voltage = ina.getBusVoltage();
    socPercent = socFromVoltage(voltage);
    coulombCount = (socPercent / 100.0) * batteryCapacityAs;
  }

  #if BAME_DEBUG
  Serial.println(F("--- Demarrage ---"));
  #endif
  lastMeasure = millis();
  lastInteraction = millis();
}

void loop() {
  unsigned long now = millis();

  // Gestion bouton
  Button b = readButton();
  if (b != BTN_NONE) {
    if (oledSleeping) {
      waitButtonRelease();
      exitOledSleep();
    } else if (b == BTN_CENTER) {
      checkLongPress();
    }
    lastInteraction = millis();
  }

  // Auto-sleep ecran apres 60s
  if (!oledSleeping && (millis() - lastInteraction >= AUTO_SLEEP_MS)) {
    enterOledSleep();
  }

  // Deep sleep auto apres 5min
  if (autoDeepSleep && (millis() - lastInteraction >= AUTO_DEEPSLEEP_MS)) {
    enterDeepSleep();
    lastMeasure = millis();
    lastInteraction = millis();
  }

  if (now - lastMeasure >= MEASURE_INTERVAL_MS) {
    updateMeasurements();
  }

  if (!oledSleeping && now - lastDisplay >= DISPLAY_INTERVAL_MS) {
    lastDisplay = now;
    gfx.tick();
    updateDisplay();
    #if BAME_DEBUG
    if (demoMode) debugSerial();
    #endif
  }
}
