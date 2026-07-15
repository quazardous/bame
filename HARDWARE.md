# Hardware

## Components

| Part | Reference | Notes |
|------|-----------|-------|
| MCU | ATmega328PB (prod) or Arduino Nano (proto) | 16MHz, 32KB flash, 2KB RAM |
| Current/voltage sensor | INA226 (I2C 0x40) | 2.5mOhm shunt, 30A max |
| Display | SSD1306 OLED 128x64 bicolor (I2C 0x3C) | Yellow (top 16px) + Blue (bottom 48px) |

Plug-and-forget: no keypad or button is fitted — the device has no runtime UI.

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
                           +-----------+
```

### Connections summary

| MCU Pin | Connected to | Notes |
|---------|-------------|-------|
| A4 (SDA) | INA226 SDA + OLED SDA | Shared I2C bus |
| A5 (SCL) | INA226 SCL + OLED SCL | Shared I2C bus |
| 5V | INA226 VCC + OLED VCC | From regulator |
| GND | All GND | Common ground |

### INA226

The INA226 measures battery voltage and current through a shunt resistor.

- **Shunt**: 2.5mOhm (0.0025 Ohm) rated for 30A+
- **VBUS**: connected to battery positive (measures 0-36V)
- **IN+ / IN-**: across the shunt resistor in the battery negative path
- Current flows: Battery(-) → IN+ → SHUNT → IN- → Load/GND

## Power

The device is powered from the monitored battery and runs continuously (always-on coulomb counting — it can't integrate while asleep). The MCU and INA226 stay awake; only the OLED sleeps (`SSD1306_DISPLAYOFF`, ~10 µA) after 2 minutes with no electrical activity, and wakes on any current or voltage change. A BMS cutoff removes power entirely, so BaMe reboots rather than observing it.

## Production board

The production target uses an ATmega328PB with a USBasp programmer. No bootloader, full 32KB flash available. Connect USBasp to the ISP header (MOSI, MISO, SCK, RST, VCC, GND).

## Prototype

Any Arduino Nano with USB-C works for prototyping. The firmware fits in the 30KB available (with bootloader). Wire the components on a breadboard following the pin assignments above.
