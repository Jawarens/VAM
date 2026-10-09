# VAM - Volt/Ampere Meter & Clamp Meter Firmware Suite

A collection of telemetry and high-speed data logging firmware built with **ESP-IDF v6.1** for **M5Stack VAMeter** and **ESP32-C3-LCDKit** with Hall-effect current sensors.

<p align="center">
  <a href="https://ko-fi.com/F0T028FQJQ">
    <img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="ko-fi" />
  </a>
</p>

---

## 📁 Repository Structure

`
VAM/
├── VAMeter-Firmware/       # M5Stack VAMeter High-Precision Meter Firmware (ESP-IDF v6.1)
├── clamp-meter/            # ESP32-C3-LCDKit LEM HO-100-S Clamp Meter & Web Server
├── download_logs.py        # Python script to download CSV telemetry logs over Wi-Fi
├── read_serial.py          # Python high-speed serial monitoring tool
└── README.md
`

---

## ⚡ 1. M5Stack VAMeter (VAMeter-Firmware)
- **Target Hardware**: M5Stack VAMeter (ESP32-S3)
- **Features**:
  - Voltage, Current, Power, and Energy measurement.
  - Native **LEDC Hardware PWM Speaker/Buzzer Driver** compatible with ESP-IDF v6.1.
  - Rotary Encoder navigation tuned for single-detent step per click.
  - SPIFFS CSV logging and USB mass storage / Wi-Fi download support.
  - Modernized component build architecture.

---

## 🧲 2. ESP32-C3 Clamp Meter (clamp-meter)
- **Target Hardware**: ESP32-C3-LCDKit with GC9A01 240x240 Circular Display
- **Sensor**: LEM HO-100-S (5V Hall Effect Sensor via 10k/20k voltage divider to GPIO4)
- **Features**:
  - Direct 240x240 DMA full-frame rendering pipeline.
  - Polarity-aware live current meter:
    - **Positive / Zero Current ($\ge 0.0\text{A}$)**: Electric Green
    - **Negative Current ($< 0.0\text{A}$)**: Crimson Red
  - Circular dial progress arc with zero UI card overlap.
  - Onboard ESP32-C3 hardware temperature monitoring.
  - Low-power WS2812 RGB LED state indicator (GPIO8).
  - Multi-level volume buzzer (Mute, Low, Med, High) on GPIO3.
  - Quick Hold Menu: Zero calibration, Reset peak, Log interval, Volume, Wi-Fi portal, Erase logs.
  - Embedded Wi-Fi Access Point (ClampMeter-WiFi) & HTTP server (http://192.168.4.1) for direct browser CSV log download.

---

## 🛠️ Build & Flash Instructions

### Prerequisites
- ESP-IDF v6.1-beta1 (or ESP-IDF v5.x / v6.x)
- Python 3.10+

### Building & Flashing M5Stack VAMeter:
`powershell
cd VAMeter-Firmware
idf.py set-target esp32s3
idf.py build
idf.py -p COM4 flash
`

### Building & Flashing ESP32-C3 Clamp Meter:
`powershell
cd clamp-meter
idf.py set-target esp32c3
idf.py build
idf.py -p COM5 flash
`

---

## ☕ Support the Project

If this project saves you time or is helpful to your electronics projects, please consider supporting development!

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/F0T028FQJQ)

---

## 📜 License
MIT License.
