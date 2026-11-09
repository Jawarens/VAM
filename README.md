# ⚡ VAM — Volt/Ampere Meter & Clamp Meter Firmware Suite

<p align="center">
  <strong>High-speed telemetry, power monitoring, and current sensing firmware for M5Stack VAMeter (ESP32-S3) and ESP32-C3-LCDKit Clamp Meter</strong>
</p>

<p align="center">
  <a href="https://ko-fi.com/F0T028FQJQ">
    <img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="ko-fi" />
  </a>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-ESP--IDF%20v5.x%20%2F%20v6.1-red.svg" alt="Platform" />
  <img src="https://img.shields.io/badge/Hardware-ESP32--S3%20%7C%20ESP32--C3-blue.svg" alt="Hardware" />
  <img src="https://img.shields.io/badge/Sensor-LEM%20HO--100--S-orange.svg" alt="Sensor" />
  <img src="https://img.shields.io/badge/Display-GC9A01%20240x240%20Circular-teal.svg" alt="Display" />
  <img src="https://img.shields.io/badge/License-MIT-green.svg" alt="License" />
</p>

---

## 📁 Repository Structure

```
VAM/
├── m5stack-vameter/       # M5Stack VAMeter High-Precision Meter Firmware (ESP32-S3)
├── VAMeter-Firmware/      # Core upstream & customized VAMeter component tree
├── clamp-meter/           # ESP32-C3-LCDKit LEM HO-100-S Clamp Meter Firmware
├── feather-clampmeter/    # Alternative ESP32-C3 clamp meter variant & web portal
├── sd-logger/             # Standalone SD-card high-speed telemetry logger
├── download_logs.py       # Python script to download CSV telemetry logs over Wi-Fi
├── read_serial.py         # Python high-speed serial monitoring tool
├── analyze_telemetry.py   # Telemetry log parser & visualization
├── analyze_todays_logs.py # Daily logging aggregator & Excel report builder
├── kopplingsschema.html   # Interactive visual wiring diagram
├── WIRING.md              # Hardware wiring specs, voltage dividers, and pinouts
└── README.md
```

---

## ⚡ 1. M5Stack VAMeter (`VAMeter-Firmware`)
- **Target Hardware**: [M5Stack VAMeter (ESP32-S3)](https://docs.m5stack.com/en/products/sku/K136)
- **Features**:
  - Precision Voltage, Current, Power, and Cumulative Energy measurement.
  - Native **LEDC Hardware PWM Speaker/Buzzer Driver** compatible with modern ESP-IDF.
  - Rotary Encoder navigation tuned for single-detent step per click.
  - SPIFFS CSV logging with USB mass storage & Wi-Fi download support.
  - Modernized component build architecture and high-performance waveform view.

---

## 🧲 2. ESP32-C3 Clamp Meter (`clamp-meter` / `feather-clampmeter`)
- **Target Hardware**: ESP32-C3-LCDKit with GC9A01 240×240 Circular Color Display
- **Sensor**: LEM HO-100-S (5V Hall Effect Sensor via 10k/20k precision voltage divider to GPIO4)
- **Features**:
  - Direct 240×240 DMA full-frame rendering pipeline.
  - Polarity-aware live current meter:
    - **Positive / Zero Current ($\ge 0.0\text{A}$)**: Electric Green
    - **Negative Current ($< 0.0\text{A}$)**: Crimson Red
  - Circular dial progress arc with zero UI card overlap.
  - Onboard ESP32-C3 hardware temperature monitoring.
  - Low-power WS2812 RGB LED state indicator (GPIO8).
  - Multi-level volume buzzer (Mute, Low, Med, High) on GPIO3.
  - Quick Hold Menu: Zero calibration, Reset peak, Log interval, Volume, Wi-Fi portal, Erase logs.
  - Embedded Wi-Fi Access Point (`ClampMeter-WiFi`) & HTTP server (`http://192.168.4.1`) for direct in-browser CSV log download.

---

## 🛠️ Build & Flash Instructions

### Prerequisites
- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/) v5.1+ or v6.1
- Python 3.10+

### Building & Flashing M5Stack VAMeter:
```powershell
cd VAMeter-Firmware
idf.py set-target esp32s3
idf.py build
idf.py -p COM4 flash
```

### Building & Flashing ESP32-C3 Clamp Meter:
```powershell
cd clamp-meter
idf.py set-target esp32c3
idf.py build
idf.py -p COM5 flash
```

---

## 🔌 Hardware Connections & Wiring

See [WIRING.md](WIRING.md) and open [kopplingsschema.html](kopplingsschema.html) in your browser for detailed schematics, resistor dividers, and terminal pinouts for the LEM HO-100-S current sensor.

---

## ☕ Support the Project

If this project saves you time or is helpful to your electronics and vehicle telemetry projects, please consider supporting development!

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/F0T028FQJQ)

---

## 📜 License

MIT License.
