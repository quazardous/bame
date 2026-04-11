#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);

  delay(1000);
  Serial.println(F("=== Chip ID ==="));

  #if defined(__AVR_ATmega328P__)
    Serial.println(F("MCU: ATmega328P"));
  #elif defined(__AVR_ATmega328__)
    Serial.println(F("MCU: ATmega328"));
  #elif defined(__AVR_ATmega168__)
    Serial.println(F("MCU: ATmega168"));
  #elif defined(__AVR_ATmega2560__)
    Serial.println(F("MCU: ATmega2560"));
  #elif defined(__AVR_ATmega32U4__)
    Serial.println(F("MCU: ATmega32U4"));
  #else
    Serial.println(F("MCU: inconnu"));
  #endif

  Serial.print(F("F_CPU: "));
  Serial.print(F_CPU / 1000000);
  Serial.println(F(" MHz"));

  Serial.print(F("LED_BUILTIN: pin "));
  Serial.println(LED_BUILTIN);

  Serial.println(F("LED clignote..."));
}

void loop() {
  digitalWrite(LED_BUILTIN, HIGH);
  delay(500);
  digitalWrite(LED_BUILTIN, LOW);
  delay(500);
  Serial.println(F("blink"));
}
