# VAM (Volt/Ampere Meter) - Complete Wiring & Hardware Guide

This document provides the definitive wiring specification, pinout matrix, and step-by-step assembly guide for the **VAM Multi-Channel Telemetry Logger** (`sd-logger`) built on the **Adafruit Feather ESP32-S3**.

---

## 📐 1. System Architecture & Buses

The system is organized into modular plug-and-play buses and dedicated interfaces:

```
                                ┌──────────────────────────────────────┐
                                │       Adafruit Feather ESP32-S3       │
                                └──────┬────────────────────┬──────────┘
                                       │                    │
              ┌────────────────────────┴─────────┐   ┌──────┴───────────────────────────┐
              │    STEMMA QT / I2C Bus           │   │     Shared High-Speed SPI Bus    │
              │  (SDA: GPIO 3, SCL: GPIO 4)      │   │   (MOSI: GPIO 35, SCK: GPIO 36)  │
              │    Power Gate: GPIO 7 (High)     │   │                                  │
              └──────────────┬───────────────────┘   └──────────────┬───────────────────┘
                             │                                      │
            ┌────────────────┼────────────────┐       ┌─────────────┴─────────────┐
            │                │                │       │                           │
     ┌──────┴──────┐  ┌──────┴──────┐  ┌──────┴─────┐ │                    ┌──────┴──────┐
     │ INA228 #1   │  │ INA228 #2   │  │ DS3231 RTC │ │ GC9A01 LCD        │ MicroSD SPI │
     │ Addr: 0x40  │  │ Addr: 0x41  │  │ Addr: 0x68 │ │ (CS: 12, DC: 9,   │ (CS: 11,    │
     │ Bus V & I   │  │ (A0 -> VS)  │  │ Timestamp  │ │  BL: 14)          │  MISO: 37)  │
     └─────────────┘  └─────────────┘  └────────────┘ └───────────────────┴─────────────┘
                             │
     ┌───────────────────────┴────────────────────────┐
     │ Analog Transducers (Scaled via 10k/20k)        │
     │ • LEM DHAB S/157 Ch1 (±50A)   -> GPIO 5 (ADC4) │
     │ • LEM DHAB S/157 Ch2 (±500A)  -> GPIO 6 (ADC5) │
     │ • LEM HO-100-S Clamp (±100A)  -> GPIO 8 (ADC7) │
     │ • Onboard VBAT Divider (2x)   -> GPIO 1 (ADC0) │
     └────────────────────────────────────────────────┘
```

---

## 🔌 2. Complete Pinout Matrix

| ESP32-S3 Pin | Feather Header | Signal | Connected Peripheral | Voltage / Level | Function |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **GPIO 7** | Pin 7 | `STEMMA_PWR` | Onboard P-MOSFET | 3.3V Digital Out | Drives gate to energize STEMMA QT bus rail |
| **GPIO 3** | SDA | `I2C_SDA` | INA1, INA2, DS3231 | 3.3V Open-Drain | 400 kHz Fast-Mode I2C Data line |
| **GPIO 4** | SCL | `I2C_SCL` | INA1, INA2, DS3231 | 3.3V Open-Drain | 400 kHz Fast-Mode I2C Clock line |
| **GPIO 35**| MOSI | `SPI2_MOSI` | MicroSD (and GC9A01 if enabled) | 3.3V Master Out | SPI2 Master Out (SD DI) |
| **GPIO 36**| SCK | `SPI2_SCLK` | MicroSD (and GC9A01 if enabled) | 3.3V Clock | SPI2 Master Clock (SD CLK) |
| **GPIO 37**| MISO | `SPI2_MISO` | MicroSD Breakout | 3.3V Master In | MicroSD Data Out (`DO`) |
| **GPIO 10**| Pin 10 | `SD_CS` | Adalogger MicroSD | 3.3V Digital Out | Dedicated MicroSD Chip Select (Active Low) |
| **GPIO 12**| Pin 12 | `ST7920_CS` / `LCD_CS` | 12864B V2.0 Pin 4 (`RS/CS`) *(or GC9A01 `CS`)* | 3.3V Digital Out | Display Chip Select (12864B: Active HIGH) |
| **GPIO 9** | Pin 9 | `ST7920_SID` / `LCD_DC` | 12864B V2.0 Pin 5 (`R/W / SID`) *(or GC9A01 `DC`)* | 3.3V Digital Out | Dedicated SPI3 Data (`MOSI`) for 12864B |
| **GPIO 11**| Pin 11 | `ST7920_CLK` | 12864B V2.0 Pin 6 (`E / CLK`) | 3.3V Digital Out | Dedicated SPI3 Clock (`SCLK`) for 12864B |
| **GPIO 14**| Pin A4 | `LCD_BL` | Display Backlight Control | 3.3V Digital Out | Backlight enable (high = ON, low = dimmed) |
| **GPIO 5** | Pin 5 | `ADC1_CH4` | LEM DHAB S/157 Ch 1 | 0.33V – 3.0V Scaled | High-Sensitivity Current Channel (±50A) |
| **GPIO 6** | Pin 6 | `ADC1_CH5` | LEM DHAB S/157 Ch 2 | 0.33V – 3.0V Scaled | High-Current Channel (±500A) |
| **GPIO 8** | A5 | `ADC1_CH7` | LEM HO-100-S Clamp | 0.33V – 3.0V Scaled | Auxiliary Clamp Current Sensor (±100A) |
| **GPIO 1** | A0 / VBAT | `ADC1_CH0` | Onboard VBAT Divider| 0.0V – 2.1V Scaled | 18650 Li-ion Battery Voltage & Curve |
| **GPIO 17**| A1 | `BTN_LEFT` | Tactile Button 1 | Active LOW (Pullup) | Short Press: Prev Page / Long Press: TARE |
| **GPIO 16**| A2 | `BTN_MENU` | Tactile Button 2 | Active LOW (Pullup) | Start / Stop Telemetry Logging to SD |
| **GPIO 15**| A3 | `BTN_RIGHT`| Tactile Button 3 | Active LOW (Pullup) | Next Telemetry Screen / View |

---

## 🛠️ 3. Step-by-Step Assembly Guide (CraftingTable Style)

### Phase 1: STEMMA QT / Qwiic I2C Chain (Zero Soldering)
STEMMA QT uses standard 4-pin JST-SH 1.0mm cables with standardized wire colors:
- **Pin 1 (Black)**: `GND`
- **Pin 2 (Red)**: `3.3V Power`
- **Pin 3 (Blue / White)**: `I2C SDA`
- **Pin 4 (Yellow)**: `I2C SCL`

1. **Connect Feather to INA228 #1**:
   - Plug Cable 1 from Feather ESP32-S3 STEMMA QT port &rarr; INA228 #1 `Port IN`.
2. **Connect INA228 #1 to INA228 #2**:
   - Plug Cable 2 from INA228 #1 `Port OUT` &rarr; INA228 #2 `Port IN`.
   - **Crucial Solder Jumper**: On INA228 #2, bridge solder pads **A0 to VS** on the back of the breakout board. This shifts its I2C address from `0x40` to `0x41`, preventing bus collisions.
3. **Connect INA228 #2 to DS3231 RTC**:
   - Plug Cable 3 from INA228 #2 `Port OUT` &rarr; DS3231 RTC `Port IN` (or wire VCC, GND, SDA, SCL).

### Phase 2: Display & MicroSD Card

#### Option A (Default, `USE_DISPLAY_ST7920 = 1`): 12864B V2.0 (ST7920 128×64, Dedicated SPI3)
MicroSD runs alone on `SPI2` (`MO/SCK/MI` + `Pin 10 CS`) while the **12864B V2.0** uses its own dedicated `SPI3` bus (zero bus contention):

| 12864B V2.0 Pin | Symbol | Connect To | Notes |
| :--- | :--- | :--- | :--- |
| **1** | `VSS` | `GND` | Common ground |
| **2** | `VDD` | `5V` *(Step-Up)* or `3.3V` | Check module label (`5V` typical; `3.3V` if 3.3V variant) |
| **3** | `V0` | Contrast Trimpot (10kΩ) | Wiper to `V0`, ends to `VOUT` (Pin 18) & `GND` (if not onboard) |
| **4** | `RS / CS` | Feather **Pin 12** (`GPIO 12`) | Chip Select (Active HIGH) |
| **5** | `R/W / SID` | Feather **Pin 9** (`GPIO 9`) | SPI3 Serial Data (`SID`) |
| **6** | `E / CLK` | Feather **Pin 11** (`GPIO 11`) | SPI3 Clock (`CLK`) |
| **15** | `PSB` | **`GND`** | **Required** to select SPI/Serial mode |
| **19** | `BLA` | `5V` (via 220Ω if needed) | Backlight (+) *(or switched via transistor from **A4 / GPIO 14**)* |
| **20** | `BLK` | `GND` | Backlight (-) |

#### Option B (`USE_DISPLAY_ST7920 = 0`): GC9A01 1.28" Round LCD + MicroSD (Shared SPI2)
1. Connect `MOSI (GPIO 35)` to GC9A01 `SDA/DIN` AND MicroSD `DI`.
2. Connect `SCK (GPIO 36)` to GC9A01 `SCL/CLK` AND MicroSD `CLK`.
3. Connect dedicated control lines:
   - Feather `GPIO 12` &rarr; GC9A01 `CS`
   - Feather `GPIO 9` &rarr; GC9A01 `DC`
   - Feather `GPIO 14` &rarr; GC9A01 `BL`
   - Feather `GPIO 10` &rarr; MicroSD `CS`
   - Feather `GPIO 37` &rarr; MicroSD `DO / MISO`

### Phase 3: LEM 5V Transducers & Voltage Dividers
LEM sensors operate from +5V and swing between $0.5\text{V}$ and $4.5\text{V}$ ($2.50\text{V} \pm 2.0\text{V}$). Because ESP32-S3 ADC inputs must not exceed $3.3\text{V}$, each analog channel must pass through a 10k/20k voltage divider:

```
LEM Signal Output (0.5V - 4.5V)
           │
         ┌─┴─┐
         │10k│ (1% Metal Film)
         └─┬─┘
           ├──────────────────────────> Scaled ADC Input (0.33V - 3.0V max)
         ┌─┴─┐                          to ESP32 GPIO 5 / GPIO 6 / GPIO 8
         │20k│ (1% Metal Film)
         └─┬─┘
           │
          GND
```

- **Divider Formula**:
  $$V_{adc} = V_{sensor} \times \frac{20\,\text{k}\Omega}{10\,\text{k}\Omega + 20\,\text{k}\Omega} = V_{sensor} \times 0.6667$$
- **Zero-Current Centering**:
  $2.50\text{V} \times 0.6667 = 1.667\text{V}$ ($1667\text{mV}$ resting point, calibrated by firmware).

### Phase 4: Navigation Buttons
1. Connect one side of all 3 tactile buttons to a shared **Ground (GND)** rail.
2. Connect individual switch terminals:
   - **Left / Tare Button**: Feather **GPIO 17 (Pin A1)**
   - **Menu / Record Button**: Feather **GPIO 16 (Pin A2)**
   - **Right / Next Button**: Feather **GPIO 15 (Pin A3)**
3. Firmware initializes these with `GPIO_PULLUP_ENABLE`. Pressing any button connects the pin to ground (Active LOW).

---

## ⚡ 4. Hardware Verification & Bring-Up Checklist

- [ ] **Power Gate Check**: Ensure firmware calls `gpio_set_level(PIN_I2C_POWER, 1)` before executing `i2c_master_probe` or initializing INA228 sensors.
- [ ] **I2C Bus Scan**: Verify serial log shows devices responding at `0x40` (INA #1), `0x41` (INA #2), and `0x68` (DS3231).
- [ ] **SD Mount**: Ensure MicroSD card is formatted as FAT32. On boot, verify log outputs `SD Card mounted at /sdcard`.
- [ ] **Current Zero Tare**: With no current flowing through the LEM aperture or INA shunts, long-press the **Left Button (1.5 seconds)** to store zero-offsets to RAM.

