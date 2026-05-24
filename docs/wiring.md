# Wiring Guide

## Overview

The ESP32 communicates with the OLED display over I2C (2 wires). The five LED indicators connect directly to GPIO pins via current-limiting resistors.

---

## OLED Display Wiring (I2C)

Most SH1106 and SSD1306 OLED modules have 4 pins: **VCC, GND, SCL, SDA**.

| OLED Pin | ESP32 Pin | Notes |
|---|---|---|
| VCC | 3.3V | Do NOT connect to 5V — will damage display |
| GND | GND | Any GND pin |
| SCL | GPIO 22 | Hardware I2C clock |
| SDA | GPIO 21 | Hardware I2C data |

> **I2C Address:** Most modules use `0x3C`. If the display stays blank, try `0x3D` — some modules ship with the address pad bridged differently. Change `OLED_ADDR` in the sketch.

---

## LED Indicator Wiring

Each LED connects from a GPIO pin → 220Ω resistor → LED → GND.

| LED Colour | Function | ESP32 GPIO | Lights when |
|---|---|---|---|
| 🟢 Green | VFR | GPIO 2 | Ceiling ≥ 3000ft AND vis ≥ 5km |
| 🔵 Blue | MVFR | GPIO 4 | Ceiling 1000–2999ft OR vis 1.6–4.9km |
| 🔴 Red | IFR | GPIO 5 | Ceiling 300–999ft OR vis 800m–1.5km |
| 🟣 Purple/White | LIFR | GPIO 18 | Ceiling < 300ft OR vis < 800m |
| 🟡 Amber | WIND | GPIO 19 | Wind ≥ 15kt OR gusts ≥ 25kt |

---

## Circuit Diagram (ASCII)

```
ESP32 Dev Module
─────────────────────────────────────────────────────────

3.3V ──────────────────────────── OLED VCC
GND  ──────────────────────────── OLED GND
GPIO 22 (SCL) ─────────────────── OLED SCL
GPIO 21 (SDA) ─────────────────── OLED SDA


GPIO 2  ──── [220Ω] ──── [GREEN  LED +] ──── GND   (VFR)
GPIO 4  ──── [220Ω] ──── [BLUE   LED +] ──── GND   (MVFR)
GPIO 5  ──── [220Ω] ──── [RED    LED +] ──── GND   (IFR)
GPIO 18 ──── [220Ω] ──── [PURPLE LED +] ──── GND   (LIFR)
GPIO 19 ──── [220Ω] ──── [AMBER  LED +] ──── GND   (WIND)

LED flat side (cathode) = GND side
LED long leg (anode)    = resistor side
```

---

## Breadboard Layout

```
                    ┌──────────────────────────────┐
                    │   SH1106 / SSD1306 OLED       │
                    │  [VCC][GND][SCL][SDA]         │
                    └──┬────┬────┬────┬─────────────┘
                       │    │    │    │
                      3.3V GND  22   21
                       │    │    │    │
┌──────────────────────┴────┴────┴────┴───────────────────────┐
│                                                             │
│   E  S  P  3  2     D  E  V     B  O  A  R  D              │
│                                                             │
│  [ 3.3V ]                                                   │
│  [ GND  ]                                                   │
│  [ 21   ] SDA ─────────────────────────────────────────     │
│  [ 22   ] SCL ─────────────────────────────────────────     │
│  [  2   ] ─── 220Ω ─── GREEN  LED ─── GND                  │
│  [  4   ] ─── 220Ω ─── BLUE   LED ─── GND                  │
│  [  5   ] ─── 220Ω ─── RED    LED ─── GND                  │
│  [ 18   ] ─── 220Ω ─── PURPLE LED ─── GND                  │
│  [ 19   ] ─── 220Ω ─── AMBER  LED ─── GND                  │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## Disabling LEDs

If you don't want to wire LEDs, set any pin to `-1` in the sketch:

```cpp
#define LED_VFR   -1   // disabled
#define LED_MVFR  -1   // disabled
#define LED_IFR   -1   // disabled
#define LED_LIFR  -1   // disabled
#define LED_WARN  -1   // disabled
```

The sketch checks for `-1` before calling `digitalWrite`, so nothing breaks.

---

## Power Supply

The ESP32 Dev Module can be powered via:

- **USB** (during development / if near a USB port) — 5V from PC or USB charger
- **VIN pin** (5V from a wall adapter or LiPo charger board)
- **3.3V pin** (direct regulated 3.3V supply — bypasses the onboard regulator)

The OLED draws ~20mA at full brightness. All five LEDs draw ~10mA each. Total current budget is well within USB power limits.

---

## Tested Hardware

| Component | Source | Notes |
|---|---|---|
| ESP32 30-pin Dev Board (ESP32-WROOM-32) | Amazon / AliExpress | Any 30-pin variant works |
| 1.3" SH1106 OLED I2C 128×64 (white) | Amazon | Most common for this project |
| 0.96" SSD1306 OLED I2C 128×64 (blue) | Amazon / Pimoroni | Smaller but works identically |
| 3mm LED assortment | Amazon | Standard 2V forward voltage |
| 220Ω resistor pack | Amazon | ¼W carbon film |

---

## Alternative OLED Modules

If your OLED has a different pinout (some have VCC/GND swapped, or an extra RESET pin):

- **RESET pin** — connect to ESP32 GPIO or leave floating (most drivers work without it)
- **VCC/GND swapped** — some cheap modules have GND on pin 1; check the silkscreen
- **4-wire SPI OLED** — requires different U8G2 constructor; see [U8g2 wiki](https://github.com/olikraus/u8g2/wiki)

---

## Notes for RC / Aviation Use

- The ESP32 is a 3.3V device — keep it away from 5V logic without a level shifter
- For cockpit/shack use, power via a 5V BEC or USB power bank
- The I2C bus is short-range (~30cm reliably on a breadboard); for longer runs, reduce I2C speed: add `Wire.setClock(100000)` in setup()
- OLED contrast can be reduced for night use: change `u8g2.setContrast(200)` to a lower value (0–255)
