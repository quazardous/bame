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

## Serial / FTDI (telemetry, and optionally flashing)

D0/D1 are free (the keypad and action button are gone), so a USB-serial adapter
gives live telemetry — and, with a bootloader, flashing too.

```
FTDI RX   <--- D1 / PD1 (MCU TX)      needed to read anything
FTDI TX   ---> D0 / PD0 (MCU RX)      only needed for serial flashing
FTDI DTR  ---> [100 nF] ---> RESET    only needed for serial flashing (auto-reset)
FTDI GND  <--> GND
FTDI VCC       DO NOT CONNECT
```

**Never connect the FTDI's VCC.** The board is powered by its own 12→5 V buck;
a second 5 V source fights it and makes the adapter's USB interface flaky (this
is exactly what the USBasp's VCC jumper did — see below). Set the FTDI to **5 V
logic**, since the MCU runs at 5 V.

**For measuring the current noise floor, unplug the USBasp first.** Each adapter
brings its own ground path back to the PC; with both connected you create a
ground loop, and the shunt resolves *microvolts* (50 mA = 125 µV across the
2.5 mΩ shunt). You would be measuring your bench, not the INA226. Otherwise the
USBasp and the FTDI coexist fine — they use different pins (SPI + RESET vs
UART), so you can flash on one and watch the boot on the other.

### Switching to serial flashing (optional)

The board already uses MiniCore, whose default upload protocol is `urclock` — a
serial bootloader. Burning it means no more ISP for everyday flashing, and the
same FTDI cable carries both the firmware and the telemetry.

1. Wire the FTDI as above, **including** TX and the DTR/100 nF auto-reset.
2. One last time with the USBasp connected:
   `pio run -e prod-bus-4s-boot -t bootloader`
   This writes the bootloader **and** the fuses (`lfuse 0xFF / hfuse 0xD6 /
   efuse 0xFD` — pinned in `platformio.ini` so the burn can't quietly undo
   EESAVE, i.e. can't go back to wiping the EEPROM on every ISP chip erase).
3. From then on: `pio run -e prod-bus-4s-serial -t upload`.

Costs: 384 B of flash for the bootloader (app space 32768 -> 32384 B, i.e. 83.4 %
used instead of 82.4 %), and ~1 s of boot delay while it waits for an upload. Note a bootloader upload only rewrites flash pages — it never
touches the EEPROM — so the learned capacity and coulomb counter always survive.

> Untested: written from the board definition, not yet run on hardware.

## Production board

The production target uses an ATmega328PB with a USBasp programmer. No bootloader, full 32KB flash available. Connect USBasp to the ISP header (MOSI, MISO, SCK, RST, VCC, GND).

## Prototype

Any Arduino Nano with USB-C works for prototyping. The firmware fits in the 30KB available (with bootloader). Wire the components on a breadboard following the pin assignments above.
