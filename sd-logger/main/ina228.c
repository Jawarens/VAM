#include "ina228.h"
#include "config_pins.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "INA228";

typedef struct {
    uint8_t addr;
    i2c_master_dev_handle_t handle;
    bool online;
} ina228_dev_ctx_t;

static ina228_dev_ctx_t s_devs[2] = {
    { .addr = INA228_I2C_ADDR_1, .handle = NULL, .online = false },
    { .addr = INA228_I2C_ADDR_2, .handle = NULL, .online = false }
};

extern i2c_master_bus_handle_t g_i2c_bus_handle;

// Shunt resistor configuration: Adafruit 5832 onboard shunt is 15 mOhm (0.015 Ohm)
#define SHUNT_RESISTOR_OHMS     0.015f
#define MAX_EXPECTED_CURRENT_A  10.0f

#define VBUS_LSB_V              0.0001953125f
#define VSHUNT_LSB_V            0.0000003125f
#define TEMP_LSB_C              0.0078125f

static float s_current_lsb = 10.0f / 524288.0f;

static esp_err_t ina228_write_reg(i2c_master_dev_handle_t handle, uint8_t reg, uint16_t value) {
    if (!handle) return ESP_ERR_INVALID_STATE;
    uint8_t buf[3];
    buf[0] = reg;
    buf[1] = (value >> 8) & 0xFF;
    buf[2] = value & 0xFF;
    return i2c_master_transmit(handle, buf, 3, 50);
}

static esp_err_t ina228_read_reg16(i2c_master_dev_handle_t handle, uint8_t reg, uint16_t *val) {
    if (!handle) return ESP_ERR_INVALID_STATE;
    uint8_t rx[2];
    esp_err_t err = i2c_master_transmit_receive(handle, &reg, 1, rx, 2, 50);
    if (err == ESP_OK) {
        *val = ((uint16_t)rx[0] << 8) | rx[1];
    }
    return err;
}

static esp_err_t ina228_read_reg24(i2c_master_dev_handle_t handle, uint8_t reg, int32_t *val) {
    if (!handle) return ESP_ERR_INVALID_STATE;
    uint8_t rx[3];
    esp_err_t err = i2c_master_transmit_receive(handle, &reg, 1, rx, 3, 50);
    if (err == ESP_OK) {
        int32_t raw = ((int32_t)rx[0] << 16) | ((int32_t)rx[1] << 8) | rx[2];
        if (raw & 0x800000) {
            raw |= 0xFF000000;
        }
        *val = raw;
    }
    return err;
}

static esp_err_t ina228_read_reg40(i2c_master_dev_handle_t handle, uint8_t reg, uint64_t *val) {
    if (!handle) return ESP_ERR_INVALID_STATE;
    uint8_t rx[5];
    esp_err_t err = i2c_master_transmit_receive(handle, &reg, 1, rx, 5, 50);
    if (err == ESP_OK) {
        uint64_t raw = 0;
        for (int i = 0; i < 5; i++) {
            raw = (raw << 8) | rx[i];
        }
        *val = raw;
    }
    return err;
}

esp_err_t ina228_init(void) {
    if (!g_i2c_bus_handle) {
        ESP_LOGE(TAG, "I2C master bus not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    int online_count = 0;

    for (int i = 0; i < 2; i++) {
        s_devs[i].online = false;
        if (s_devs[i].handle) {
            i2c_master_bus_rm_device(s_devs[i].handle);
            s_devs[i].handle = NULL;
        }

        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = s_devs[i].addr,
            .scl_speed_hz = I2C_FREQ_HZ,
        };

        esp_err_t ret = i2c_master_bus_add_device(g_i2c_bus_handle, &dev_cfg, &s_devs[i].handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to register I2C device at 0x%02X: %s", s_devs[i].addr, esp_err_to_name(ret));
            continue;
        }

        uint16_t manuf_id = 0, dev_id = 0;
        ret = ina228_read_reg16(s_devs[i].handle, INA228_REG_MANUF_ID, &manuf_id);
        if (ret != ESP_OK || manuf_id != 0x5449) {
            ESP_LOGW(TAG, "INA228 #%d at 0x%02X not responding (Manuf ID: 0x%04X, expected 0x5449)",
                     i + 1, s_devs[i].addr, manuf_id);
            i2c_master_bus_rm_device(s_devs[i].handle);
            s_devs[i].handle = NULL;
            continue;
        }

        ina228_read_reg16(s_devs[i].handle, INA228_REG_DEVICE_ID, &dev_id);
        ESP_LOGI(TAG, "INA228 #%d detected at 0x%02X (Manuf: 0x%04X, DevID: 0x%04X)",
                 i + 1, s_devs[i].addr, manuf_id, dev_id);

        // Reset device
        ina228_write_reg(s_devs[i].handle, INA228_REG_CONFIG, 0x8000);
        vTaskDelay(pdMS_TO_TICKS(10));

        // ADC Config: Continuous mode for VBUS, VSHUNT, TEMP with 1052us conversion and 16x averaging
        uint16_t adc_cfg = (0x0F << 12) | (0x03 << 9) | (0x03 << 6) | (0x03 << 3) | 0x02;
        ina228_write_reg(s_devs[i].handle, INA228_REG_ADC_CONFIG, adc_cfg);

        double shunt_cal = 13107.2e6 * (double)s_current_lsb * (double)SHUNT_RESISTOR_OHMS;
        uint16_t cal_reg = (uint16_t)round(shunt_cal);
        ina228_write_reg(s_devs[i].handle, INA228_REG_SHUNT_CAL, cal_reg);

        s_devs[i].online = true;
        online_count++;
        ESP_LOGI(TAG, "INA228 #%d (0x%02X) initialized successfully", i + 1, s_devs[i].addr);
    }

    return (online_count > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool ina228_is_ch_online(uint8_t ch) {
    if (ch >= 2) return false;
    return s_devs[ch].online;
}

esp_err_t ina228_read_ch(uint8_t ch, ina228_data_t *data) {
    if (ch >= 2 || !data || !s_devs[ch].online || !s_devs[ch].handle) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_dev_handle_t handle = s_devs[ch].handle;
    int32_t raw_vbus = 0, raw_vshunt = 0, raw_curr = 0;
    int32_t raw_power = 0;
    uint16_t raw_temp = 0;
    uint64_t raw_energy = 0;

    if (ina228_read_reg24(handle, INA228_REG_VBUS, &raw_vbus) == ESP_OK) {
        data->bus_voltage_v = (float)(raw_vbus >> 4) * VBUS_LSB_V;
    }

    if (ina228_read_reg24(handle, INA228_REG_VSHUNT, &raw_vshunt) == ESP_OK) {
        data->shunt_voltage_mv = (float)(raw_vshunt >> 4) * (VSHUNT_LSB_V * 1000.0f);
    }

    if (ina228_read_reg24(handle, INA228_REG_CURRENT, &raw_curr) == ESP_OK) {
        data->current_a = (float)(raw_curr >> 4) * s_current_lsb;
    }

    if (ina228_read_reg24(handle, INA228_REG_POWER, &raw_power) == ESP_OK) {
        data->power_w = (float)raw_power * (3.2f * s_current_lsb);
    }

    if (ina228_read_reg40(handle, INA228_REG_ENERGY, &raw_energy) == ESP_OK) {
        double energy_joules = (double)raw_energy * (16.0 * 3.2 * (double)s_current_lsb);
        data->energy_wh = (float)(energy_joules / 3600.0);
    }

    if (ina228_read_reg16(handle, INA228_REG_DIETEMP, &raw_temp) == ESP_OK) {
        data->die_temp_c = (float)((int16_t)raw_temp >> 4) * TEMP_LSB_C;
    }

    return ESP_OK;
}

esp_err_t ina228_read(ina228_data_t *data) {
    return ina228_read_ch(0, data);
}

esp_err_t ina228_reset_energy(uint8_t ch) {
    if (ch >= 2 || !s_devs[ch].online || !s_devs[ch].handle) {
        return ESP_ERR_INVALID_STATE;
    }
    uint16_t cfg = 0;
    ina228_read_reg16(s_devs[ch].handle, INA228_REG_CONFIG, &cfg);
    cfg |= (1 << 14);
    return ina228_write_reg(s_devs[ch].handle, INA228_REG_CONFIG, cfg);
}
