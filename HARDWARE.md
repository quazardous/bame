# Hardware

## Components

| Part | Reference | Notes |
|------|-----------|-------|
| MCU | ATmega328PB (prod) or Arduino Nano (proto) | 16MHz, 32KB flash, 2KB RAM |
| Current/voltage sensor | INA226 (I2C 0x40) | 2.5mOhm shunt, 30A max |
| Display | SSD1306 OLED 128x64 bicolor (I2C 0x3C) | Yellow (top 16px) + Blue (bottom 48px) |
| Keypad | Foxeer Key23 | 5-button analog on A3 with 10K pullup |
| Action button | Momentary NO pushbutton | Pin D2, pulled to GND, used for wake-up (INT0) |

## Wiring

### I2C bus (shared)

| Signal | Pin |
|--------|-----|
| SDA | A4 |
| SCL | A5 |

Both INA226 and SSD1306 share the I2C bus.

### INA226

The INA226 measures battery voltage and current through a shunt resistor.

- **Shunt**: 2.5mOhm (0.0025 Ohm) rated for 30A+
- **Bus voltage**: connected to battery positive (measures 0-36V)
- **Shunt inputs**: across the shunt resistor in the battery negative path

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
