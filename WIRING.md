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
| **GPIO 35**| MOSI | `SPI_MOSI` | GC9A01 LCD, MicroSD | 3.3V Master Out | Shared SPI Master Out (Display DIN & SD DI) |
| **GPIO 36**| SCK | `SPI_SCLK` | GC9A01 LCD, MicroSD | 3.3V Clock | Shared SPI Master Clock (Display CLK & SD CLK) |
| **GPIO 37**| MISO | `SPI_MISO` | MicroSD Breakout | 3.3V Master In | MicroSD Data Out (`DO`) |
| **GPIO 12**| Pin 12 | `LCD_CS` | GC9A01 Round Display | 3.3V Digital Out | Dedicated Display Chip Select (Active Low) |
| **GPIO 9** | Pin 9 | `LCD_DC` | GC9A01 Round Display | 3.3V Digital Out | Display Command (0) / Data (1) Select |
| **GPIO 14**| Pin 14 | `LCD_BL` | GC9A01 Round Display | 3.3V LEDC PWM | Hardware Backlight Dimming |
| **GPIO 11**| Pin 11 | `SD_CS` | MicroSD Breakout | 3.3V Digital Out | Dedicated MicroSD Chip Select (Active Low) |
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

### Phase 2: Shared SPI Display & MicroSD Card
The GC9A01 LCD and MicroSD card share high-speed data lines while maintaining independent Chip Selects:
1. Connect `MOSI (GPIO 35)` to GC9A01 `SDA/DIN` AND MicroSD `DI`.
2. Connect `SCK (GPIO 36)` to GC9A01 `SCL/CLK` AND MicroSD `CLK`.
3. Connect dedicated control lines:
   - Feather `GPIO 12` &rarr; GC9A01 `CS`
   - Feather `GPIO 9` &rarr; GC9A01 `DC`
   - Feather `GPIO 14` &rarr; GC9A01 `BL`
   - Feather `GPIO 11` &rarr; MicroSD `CS`
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

