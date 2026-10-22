#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Feather ESP32-S3 Onboard Power Gate for STEMMA QT / I2C
#define PIN_I2C_POWER           7  // Must be driven HIGH to power STEMMA QT devices!

// I2C Pin Definitions (Feather STEMMA QT)
#define I2C_PORT_NUM            I2C_NUM_0
#define PIN_I2C_SDA             3
#define PIN_I2C_SCL             4
#define I2C_FREQ_HZ             400000

// SPI Display Pins (GC9A01 1.28" Round LCD)
#define PIN_LCD_MOSI            35
#define PIN_LCD_SCLK            36
#define PIN_LCD_CS              12
#define PIN_LCD_DC              9
#define PIN_LCD_RST             -1
#define PIN_LCD_BL              14

// SPI MicroSD Card Pins (Adafruit 254 Breakout)
#define PIN_SD_MOSI             35
#define PIN_SD_MISO             37
#define PIN_SD_SCLK             36
#define PIN_SD_CS               11

// Analog LEM Sensors Pin Definitions
#define PIN_DHAB_CH1_ADC        ADC_CHANNEL_4 // GPIO 5 (DHAB S/157 High-Sensitivity +/-50A)
#define PIN_DHAB_CH2_ADC        ADC_CHANNEL_5 // GPIO 6 (DHAB S/157 High-Current +/-500A)
#define PIN_HO100_ADC           ADC_CHANNEL_7 // GPIO 8 / Pin A5 on Feather (LEM HO-100-S +/-100A)
#define PIN_BATTERY_ADC         ADC_CHANNEL_0 // GPIO 1 (Feather Onboard VBAT Divider)

// 3 User Navigation Buttons (Active Low to GND)
#define PIN_BUTTON_LEFT         17 // Pin A1 on Feather (Short: Prev Page, Long 1.5s: ZERO/TARE)
#define PIN_BUTTON_MENU         16 // Pin A2 on Feather (Start / Stop Logging)
#define PIN_BUTTON_RIGHT        15 // Pin A3 on Feather (Next Page)

// Multi-Channel Telemetry Record Structure
typedef struct {
    uint32_t sample_idx;
    int64_t  timestamp_unix_ms;
    char     timestamp_iso[32]; // "YYYY-MM-DD HH:MM:SS.mmm"
    
    // Sensor 1: INA228 #1 (0x40 default)
    float    ina1_bus_voltage_v;
    float    ina1_current_a;
    float    ina1_power_w;
    float    ina1_energy_wh;
    float    ina1_shunt_mv;
    float    ina1_die_temp_c;

    // Sensor 2: INA228 #2 (0x41, A0 bridged)
    float    ina2_bus_voltage_v;
    float    ina2_current_a;
    float    ina2_power_w;
    float    ina2_energy_wh;
    float    ina2_shunt_mv;
    float    ina2_die_temp_c;

    // Sensor 3: LEM DHAB S/157 Dual-Range Hall Sensor
    float    dhab_current_a;
    float    dhab_ch1_50a_raw_v;
    float    dhab_ch2_500a_raw_v;
    bool     dhab_in_high_range;

    // Sensor 4: LEM HO-100-S Auxiliary Clamp
    float    ho100_current_a;
    float    ho100_raw_v;

    // Combined System Metrics
    float    total_system_current_a;
    float    total_power_w;
    float    battery_v;
    uint8_t  battery_pct;
} telemetry_record_t;

// Logging Mode Enum
typedef enum {
    LOG_MODE_CONTINUOUS_UNTIL_DEAD = 0,
    LOG_MODE_TIMED = 1
} log_mode_t;

// Global System State
typedef struct {
    log_mode_t   active_mode;
    uint32_t     timed_duration_sec;
    uint32_t     elapsed_sec;
    bool         is_logging;
    bool         sd_mounted;
    bool         ina1_online;
    bool         ina2_online;
    bool         low_battery_warning;
    uint8_t      ui_screen; // 0: Overview, 1: Dual INA, 2: LEM Hall, 3: System Status
    uint32_t     records_written;
    uint32_t     queue_overflow_count;
    char         current_log_filename[64];
    
    // Zero / Tare Functionality
    bool         is_zeroed;
    float        ina1_zero_offset_a;
    float        ina2_zero_offset_a;
    float        dhab_zero_offset_a;
    float        ho100_zero_offset_a;
    int          zero_popup_timer; // Countdown for popup message (frames)
    
    telemetry_record_t latest_telemetry;
} system_state_t;

extern system_state_t g_sys_state;

#ifdef __cplusplus
}
#endif
