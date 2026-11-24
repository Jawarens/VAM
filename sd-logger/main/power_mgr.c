#include "power_mgr.h"
#include "config_pins.h"
#include "driver/i2c_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_pm.h"
#include <math.h>


static const char *TAG = "POWER_MGR";

// Adafruit Feather ESP32-S3 has an onboard MAX17048 LiPo/Li-ion Fuel Gauge on
// I2C address 0x36
#define MAX17048_I2C_ADDR 0x36
#define MAX17048_REG_VCELL 0x02
#define MAX17048_REG_SOC 0x04
#define MAX17048_REG_MODE 0x06
#define MAX17048_REG_VERSION 0x08
#define MAX17048_REG_CRATE 0x16

extern i2c_master_bus_handle_t g_i2c_bus_handle;
static i2c_master_dev_handle_t s_max17048_handle = NULL;
static bool s_has_max17048 = false;

// Safe cutoff threshold before 18650 BMS cuts out
#define CRITICAL_BATT_CUTOFF_V 3.10f

#define BATT_HIST_LEN 8
static float s_vcell_hist[BATT_HIST_LEN] = {0};
static uint8_t s_hist_count = 0;
static uint8_t s_hist_idx = 0;
static bool s_battery_present = false;

static esp_err_t max17048_read_reg16(uint8_t reg, uint16_t *val) {
  if (!s_max17048_handle)
    return ESP_ERR_INVALID_STATE;
  uint8_t rx[2];
  esp_err_t err =
      i2c_master_transmit_receive(s_max17048_handle, &reg, 1, rx, 2, 50);
  if (err == ESP_OK) {
    *val = ((uint16_t)rx[0] << 8) | rx[1];
  }
  return err;
}

esp_err_t power_mgr_init(void) {
  s_has_max17048 = false;

  if (g_i2c_bus_handle) {
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MAX17048_I2C_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(g_i2c_bus_handle, &dev_cfg,
                                              &s_max17048_handle);
    if (ret == ESP_OK) {
      uint16_t ver = 0;
      if (max17048_read_reg16(MAX17048_REG_VERSION, &ver) == ESP_OK &&
          ver != 0xFFFF && ver != 0x0000) {
        s_has_max17048 = true;
        ESP_LOGI(
            TAG,
            "MAX17048 Li-ion Fuel Gauge initialized at 0x36 (Version: 0x%04X)",
            ver);
      } else {
        ESP_LOGW(TAG,
                 "MAX17048 device attached at 0x36, but version read 0x%04X",
                 ver);
        s_has_max17048 = true; // Still attempt to read
      }
    } else {
      ESP_LOGW(TAG, "Failed to register MAX17048 I2C device: %s",
               esp_err_to_name(ret));
    }
  }

  ESP_LOGI(TAG, "Power Manager initialized (Cutoff: %.2fV, FuelGauge: %s)",
           CRITICAL_BATT_CUTOFF_V, s_has_max17048 ? "ONLINE" : "OFFLINE");
  return ESP_OK;
}

esp_err_t power_mgr_read_battery(battery_status_t *status) {
  if (!status)
    return ESP_ERR_INVALID_STATE;

  bool usb_plugged = usb_serial_jtag_is_connected();

  if (s_has_max17048 && s_max17048_handle) {
    uint16_t raw_vcell = 0, raw_soc = 0, raw_crate = 0;
    if (max17048_read_reg16(MAX17048_REG_VCELL, &raw_vcell) == ESP_OK) {
      // MAX17048 VCELL: 78.125 µV per LSB (16-bit)
      float vcell = (float)raw_vcell * 0.000078125f;

      // If cell voltage is under 2.50V, no battery is connected to JST
      if (vcell < 2.50f) {
        s_hist_count = 0;
        s_hist_idx = 0;
        s_battery_present = false;
        status->battery_voltage_v = 0.0f;
        status->state_of_charge_pct = 0;
        status->is_charging = false;
        status->is_critical_low = false;
        status->battery_present = false;
        return ESP_OK;
      }

      // Push into chronological ring buffer
      s_vcell_hist[s_hist_idx] = vcell;
      s_hist_idx = (s_hist_idx + 1) % BATT_HIST_LEN;
      if (s_hist_count < BATT_HIST_LEN)
        s_hist_count++;

      if (!usb_plugged && vcell < 3.90f) {
        // Running off battery without USB -> battery is definitely present
        s_battery_present = true;
      } else if (s_hist_count >= 4) {
        float ordered[BATT_HIST_LEN];
        int start = (s_hist_count < BATT_HIST_LEN) ? 0 : s_hist_idx;
        for (int i = 0; i < s_hist_count; i++) {
          ordered[i] = s_vcell_hist[(start + i) % BATT_HIST_LEN];
        }

        float v_min = ordered[0], v_max = ordered[0];
        float max_step = 0.0f;
        int reversals = 0;
        int prev_dir = 0;

        for (int i = 1; i < s_hist_count; i++) {
          if (ordered[i] < v_min)
            v_min = ordered[i];
          if (ordered[i] > v_max)
            v_max = ordered[i];
          float dv = ordered[i] - ordered[i - 1];
          float abs_dv = fabsf(dv);
          if (abs_dv > max_step)
            max_step = abs_dv;
          if (abs_dv >= 0.012f) {
            int dir = (dv > 0.0f) ? 1 : -1;
            if (prev_dir != 0 && dir != prev_dir) {
              reversals++;
            }
            prev_dir = dir;
          }
        }

        float p2p = v_max - v_min;
        if (v_max >= 3.90f && p2p >= 0.035f &&
            (reversals >= 1 || max_step >= 0.025f)) {
          // Characteristic MCP73831 capacitor ripple when no battery is
          // attached
          s_battery_present = false;
        } else if (s_hist_count >= 6 && p2p < 0.020f && max_step < 0.012f) {
          s_battery_present = true;
        }
      } else if (!usb_plugged) {
        s_battery_present = true;
      }

      if (!s_battery_present) {
        status->battery_voltage_v = 0.0f;
        status->state_of_charge_pct = 0;
        status->is_charging = false;
        status->is_critical_low = false;
        status->battery_present = false;
        return ESP_OK;
      }

      status->battery_present = true;
      status->battery_voltage_v = vcell;

      if (max17048_read_reg16(MAX17048_REG_SOC, &raw_soc) == ESP_OK) {
        // MAX17048 SOC: high byte is %, low byte is 1/256%
        float soc_f = (float)raw_soc / 256.0f;
        if (soc_f > 100.0f)
          soc_f = 100.0f;
        if (soc_f < 0.0f)
          soc_f = 0.0f;
        status->state_of_charge_pct = (uint8_t)(soc_f + 0.5f);
      } else {
        status->state_of_charge_pct = 0;
      }

      if (max17048_read_reg16(MAX17048_REG_CRATE, &raw_crate) == ESP_OK) {
        int16_t crate = (int16_t)raw_crate;
        status->is_charging = usb_plugged || (crate > 1);
      } else {
        status->is_charging = usb_plugged || (vcell >= 4.20f);
      }

      status->is_critical_low = (vcell <= CRITICAL_BATT_CUTOFF_V);
      return ESP_OK;
    }
  }

  // Fallback if fuel gauge read fails (likely no battery attached)
  s_hist_count = 0;
  s_hist_idx = 0;
  s_battery_present = false;
  status->battery_voltage_v = 0.0f;
  status->state_of_charge_pct = 0;
  status->is_charging = false;
  status->is_critical_low = false;
  status->battery_present = false;
  return ESP_OK;
}

void power_mgr_set_cpu_freq_low_power(bool enable_low_power) {
#if CONFIG_PM_ENABLE
  esp_pm_config_t pm_cfg = {
      .max_freq_mhz = enable_low_power ? 80 : 240,
      .min_freq_mhz = 40,
      .light_sleep_enable = false,
  };
  esp_pm_configure(&pm_cfg);
  ESP_LOGI(TAG, "CPU Max Frequency set to %d MHz", enable_low_power ? 80 : 240);
#endif
}
