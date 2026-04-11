# Hardware

## Components

| Part | Reference | Notes |
|------|-----------|-------|
| MCU | ATmega328PB (prod) or Arduino Nano (proto) | 16MHz, 32KB flash, 2KB RAM |
| Current/voltage sensor | INA226 (I2C 0x40) | 2.5mOhm shunt, 30A max |
| Display | SSD1306 OLED 128x64 bicolor (I2C 0x3C) | Yellow (top 16px) + Blue (bottom 48px) |
| Keypad | Foxeer Key23 | 5-button analog on A3 with 10K pullup |
| Action button | Momentary NO pushbutton | Pin D2, pulled to GND, used for wake-up (INT0) |

## Wiring diagram

```
                          LFP Battery (12V/24V)
                          (+)             (-)
                           |               |
                           |          +---------+
                           |          | SHUNT   |
                           |          | 2.5mOhm |
                           |          +---------+
                           |           |       |
                           |         IN+     IN-    INA226
                           +---VBUS---+       |     module
                           |          | SDA --+---> A4 (MCU)
                           |          | SCL --+---> A5 (MCU)
                           |          | VCC --+---> 5V
                           |          | GND --+---> GND
                           |          +-------+
                           |               |
                           |     +---------+---------+
                           |     |                   |
                           |  +--+--+          +-----+-----+
                           |  | REG |          |   LOAD     |
                           |  | 5V  |          +-----+-----+
                           |  +--+--+                |
                           |     |                   |
                           +-----+-------------------+
                                 |
                           +-----+-----+
                           |  Arduino  |
                           |  Nano /   |
                           | ATmega328 |
                           |           |
                           |  A4 = SDA |---+--- SSD1306 OLED (0x3C)
                           |  A5 = SCL |---+    (shared I2C bus)
                           |           |
                           |  A3 ------|--> Foxeer Key23 (analog)
                           |           |      + 10K pullup to VCC
                           |           |
                           |  D2 ------|--> Action button (NO to GND)
                           |           |      (internal pullup)
                           +-----------+
```

### Connections summary

| MCU Pin | Connected to | Notes |
|---------|-------------|-------|
| A4 (SDA) | INA226 SDA + OLED SDA | Shared I2C bus |
| A5 (SCL) | INA226 SCL + OLED SCL | Shared I2C bus |
| A3 | Foxeer Key23 signal | 10K pullup to VCC |
| D2 | Action button | NO to GND, INT0 wake-up |
| 5V | INA226 VCC + OLED VCC | From regulator |
| GND | All GND | Common ground |

### INA226

The INA226 measures battery voltage and current through a shunt resistor.

- **Shunt**: 2.5mOhm (0.0025 Ohm) rated for 30A+
- **VBUS**: connected to battery positive (measures 0-36V)
- **IN+ / IN-**: across the shunt resistor in the battery negative path
- Current flows: Battery(-) → IN+ → SHUNT → IN- → Load/GND

### Keypad (Foxeer Key23)

Analog resistor ladder on pin A3. Each button produces a different ADC value. Calibrated at boot by holding a button for 5 seconds.

Default ADC values (with 10K pullup):
- CENTER: 838
- UP: 616
- DOWN: 1
- LEFT: 748
- RIGHT: 416

### Action button

Simple NO pushbutton between D2 and GND. Internal pullup enabled. Used for:
- Wake from deep sleep (INT0 interrupt)
- Center button alternative

## Power

The device is powered from the monitored battery. In deep sleep (eco mode), the MCU draws ~5uA. The OLED and INA226 are powered down. Periodic wake-ups (5min to 1h) measure voltage to track battery state.

## Production board

The production target uses an ATmega328PB with a USBasp programmer. No bootloader, full 32KB flash available. Connect USBasp to the ISP header (MOSI, MISO, SCK, RST, VCC, GND).

## Prototype

Any Arduino Nano with USB-C works for prototyping. The firmware fits in the 30KB available (with bootloader). Wire the components on a breadboard following the pin assignments above.
