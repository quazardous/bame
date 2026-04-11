# BAME - Battery Monitor

Firmware for a LiFePO4 12V battery monitor with automatic capacity calibration. Voltage range auto-calibrates from actual battery readings.

## Features

- **Auto capacity calibration** using exponential doubling: estimates converge with each discharge/rest cycle
- **SOC estimation** from voltage lookup + coulomb counting + rest blending
- **INA226 current auto-zero** eliminates offset drift
- **Eco mode** (deep sleep) with adaptive wake interval
- **OLED display** with SOC gauge, voltage, current, power, time remaining
- **Configurable** cell count (1-16S), nominal capacity, eco mode
- **Calibration simulation** tool for parameter validation

## Screenshots

### Main display — discharging

```
+------------------------------+
|############ 64% ############|  <- yellow: SOC gauge
+------------------------------+
  13.2V                  51Ah    <- voltage + remaining Ah
  48W                   3.7A     <- power + current
  < 13:47              > 1.2Ah   <- time left + cal counter (play = accumulating)
```

### Main display — charging

```
+------------------------------+
|##################### 89% ####|
+------------------------------+
  14.1V                  71Ah
  42W                  -3.0A
  > 02:15              [=== ]    <- charge time + battery animation
```

### Main display — calibration blocked (needs rest)

```
+------------------------------+
|############ 64% ############|
+------------------------------+
  13.2V                  51Ah
  0W                    0.0A
  (no current — time hidden)     0.8Ah  <- blinks: waiting for 5s stable rest
```

### Settings menu

```
+------------------------------+
|         BAME V1.8            |
+------------------------------+
  Capacity              80Ah
  Cells                   4S
  Eco mode               OFF
> Info cal
  Reset ALL
```

### Editing a value

```
+------------------------------+
|         BAME V1.8            |
+------------------------------+
  Capacity            [85Ah]  <- brackets = editing, UP/DN to change
  Cells                   4S
  Eco mode               OFF
> Info cal
  Reset ALL
```

### Info cal page

```
+------------------------------+
|         INFO CAL             |
+------------------------------+
Cap:82Ah [80]                    <- estimated [nominal]
1.2/2.4Ah (8%)                   <- segment/target (delta SOC%)
13.28>13.21V                     <- start>end voltage
14.58/10.02V                     <- Vmax/Vmin auto-calibrated
```

## How it works

### Calibration

The system measures energy consumed (coulombs) between two rest voltage readings. It starts with a 2-minute segment and doubles the target each time. Each estimate is weighted by its delta SOC — large segments have more influence. The capacity starts at the nominal value and converges continuously.

A segment is invalidated if sustained charging (>1A for >10s) is detected. Brief current spikes from load cycling (compressor, etc.) are tolerated.

### SOC estimation

At rest (current < 0.3A for 5s), the SOC blends 8% toward the voltage-based estimate. This corrects coulomb counting drift while avoiding jumps on the flat LFP voltage curve.

### Voltage calibration

Vmin and Vmax converge toward observed rest voltages. This adapts the SOC curve to the actual battery without changing the LFP curve shape.

## Build

Requires [PlatformIO](https://platformio.org/).

```bash
# Prototype (Arduino Nano)
pio run -e nano -t upload

# Production (ATmega328PB + USBasp)
pio run -e prod -t upload
```

## Controls

| Action | Effect |
|--------|--------|
| Hold center 0.5s | Open settings menu |
| Hold center 3s | Enter deep sleep |
| Hold any button at boot 5s | Keypad calibration |
| UP/DOWN in menu | Navigate / change value |
| CENTER in menu | Enter edit / save |
| LEFT in menu | Cancel edit / go back |

## License

MIT — see [LICENSE.txt](LICENSE.txt)
