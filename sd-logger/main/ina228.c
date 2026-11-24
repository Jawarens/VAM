#include "ina228.h"
#include "config_pins.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "INA228";

typedef struct {
    uint8_t addr;
    i2c_master_dev_handle_t handle;
    bool online;
    uint16_t cal_reg;
    uint8_t err_count;
    uint32_t read_count;
} ina228_dev_ctx_t;

static ina228_dev_ctx_t s_devs[2] = {
    { .addr = INA228_I2C_ADDR_1, .handle = NULL, .online = false, .cal_reg = 0, .err_count = 0, .read_count = 0 },
    { .addr = INA228_I2C_ADDR_2, .handle = NULL, .online = false, .cal_reg = 0, .err_count = 0, .read_count = 0 }
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

static esp_err_t ina228_read_reg24_u(i2c_master_dev_handle_t handle, uint8_t reg, uint32_t *val) {
    if (!handle) return ESP_ERR_INVALID_STATE;
    uint8_t rx[3];
    esp_err_t err = i2c_master_transmit_receive(handle, &reg, 1, rx, 3, 50);
    if (err == ESP_OK) {
        *val = ((uint32_t)rx[0] << 16) | ((uint32_t)rx[1] << 8) | rx[2];
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

static esp_err_t ina228_try_attach_and_config(uint8_t ch, uint8_t addr) {
    if (i2c_master_probe(g_i2c_bus_handle, addr, 50) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_FREQ_HZ,
    };

    i2c_master_dev_handle_t handle = NULL;
    esp_err_t ret = i2c_master_bus_add_device(g_i2c_bus_handle, &dev_cfg, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register I2C device at 0x%02X: %s", addr, esp_err_to_name(ret));
        return ret;
    }

    uint16_t manuf_id = 0, dev_id = 0;
    ret = ina228_read_reg16(handle, INA228_REG_MANUF_ID, &manuf_id);
    if (ret != ESP_OK || manuf_id != 0x5449) {
        ESP_LOGW(TAG, "Addr 0x%02X not INA228 (ret=%s, Manuf=0x%04X)", addr, esp_err_to_name(ret), manuf_id);
        i2c_master_bus_rm_device(handle);
        return ESP_ERR_NOT_FOUND;
    }

    ret = ina228_read_reg16(handle, INA228_REG_DEVICE_ID, &dev_id);
    if (ret != ESP_OK || (dev_id >> 4) != 0x228) {
        ESP_LOGW(TAG, "Addr 0x%02X unexpected DevID: 0x%04X", addr, dev_id);
        i2c_master_bus_rm_device(handle);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "INA228 #%d detected at 0x%02X (Manuf: 0x%04X, DevID: 0x%04X)",
             ch + 1, addr, manuf_id, dev_id);

    // Soft reset and configure
    ret = ina228_write_reg(handle, INA228_REG_CONFIG, 0x8000);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "INA228 #%d reset failed at 0x%02X: %s", ch + 1, addr, esp_err_to_name(ret));
        i2c_master_bus_rm_device(handle);
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    // ADC Config: Continuous mode for VBUS, VSHUNT, TEMP with 1052us conversion and 16x averaging
    uint16_t adc_cfg = (0x0F << 12) | (0x03 << 9) | (0x03 << 6) | (0x03 << 3) | 0x02;
    ret = ina228_write_reg(handle, INA228_REG_ADC_CONFIG, adc_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "INA228 #%d ADC_CONFIG write failed: %s", ch + 1, esp_err_to_name(ret));
        i2c_master_bus_rm_device(handle);
        return ret;
    }

    double shunt_cal = 13107.2e6 * (double)s_current_lsb * (double)SHUNT_RESISTOR_OHMS;
    uint16_t cal_reg = (uint16_t)round(shunt_cal);
    ret = ina228_write_reg(handle, INA228_REG_SHUNT_CAL, cal_reg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "INA228 #%d SHUNT_CAL write failed: %s", ch + 1, esp_err_to_name(ret));
        i2c_master_bus_rm_device(handle);
        return ret;
    }

    uint16_t cal_verify = 0;
    if (ina228_read_reg16(handle, INA228_REG_SHUNT_CAL, &cal_verify) != ESP_OK || cal_verify != cal_reg) {
        ESP_LOGW(TAG, "INA228 #%d SHUNT_CAL readback mismatch (wrote 0x%04X, read 0x%04X)",
                 ch + 1, cal_reg, cal_verify);
        i2c_master_bus_rm_device(handle);
        return ESP_FAIL;
    }

    s_devs[ch].addr = addr;
    s_devs[ch].handle = handle;
    s_devs[ch].cal_reg = cal_reg;
    s_devs[ch].err_count = 0;
    s_devs[ch].read_count = 0;
    s_devs[ch].online = true;
    ESP_LOGI(TAG, "INA228 #%d (0x%02X) initialized successfully (CAL=0x%04X)", ch + 1, addr, cal_reg);
    return ESP_OK;
}

esp_err_t ina228_probe_and_init_ch(uint8_t ch) {
    if (ch >= 2 || !g_i2c_bus_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_devs[ch].online && s_devs[ch].handle) {
        return ESP_OK;
    }

    if (s_devs[ch].handle) {
        i2c_master_bus_rm_device(s_devs[ch].handle);
        s_devs[ch].handle = NULL;
    }
    s_devs[ch].online = false;

    uint8_t default_addr = (ch == 0) ? INA228_I2C_ADDR_1 : INA228_I2C_ADDR_2;
    uint8_t other_ch = (ch == 0) ? 1 : 0;

    // 1. Try expected default address first (skip if owned by the other online channel)
    if (!(s_devs[other_ch].online && s_devs[other_ch].addr == default_addr)) {
        if (ina228_try_attach_and_config(ch, default_addr) == ESP_OK) {
            return ESP_OK;
        }
    }

    // 2. Fallback scan across full INA228 address space (0x40..0x4F)
    for (uint8_t addr = 0x40; addr <= 0x4F; addr++) {
        if (addr == default_addr) continue;
        if (s_devs[other_ch].online && s_devs[other_ch].addr == addr) continue;
        if (ina228_try_attach_and_config(ch, addr) == ESP_OK) {
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t ina228_init(void) {
    if (!g_i2c_bus_handle) {
        ESP_LOGE(TAG, "I2C master bus not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    int online_count = 0;
    for (uint8_t i = 0; i < 2; i++) {
        s_devs[i].online = false;
        if (s_devs[i].handle) {
            i2c_master_bus_rm_device(s_devs[i].handle);
            s_devs[i].handle = NULL;
        }
        if (ina228_probe_and_init_ch(i) == ESP_OK) {
            online_count++;
        } else {
            ESP_LOGW(TAG, "INA228 #%d not found on I2C bus (will auto-retry in background)", i + 1);
        }
    }

    return (online_count > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool ina228_is_ch_online(uint8_t ch) {
    if (ch >= 2) return false;
    return s_devs[ch].online;
}

uint8_t ina228_get_addr(uint8_t ch) {
    if (ch >= 2) return 0;
    return s_devs[ch].addr;
}

void ina228_i2c_scan(void) {
    if (!g_i2c_bus_handle) {
        printf("ERROR: I2C bus not initialized\n");
        return;
    }
    printf("Scanning I2C bus (0x03..0x77) at %d Hz...\n", I2C_FREQ_HZ);
    int found = 0;
    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        if (i2c_master_probe(g_i2c_bus_handle, addr, 50) == ESP_OK) {
            found++;
            if (addr >= 0x40 && addr <= 0x4F) {
                uint16_t manuf = 0, dev = 0;
                // Check if already registered
                i2c_master_dev_handle_t h = NULL;
                bool temp_handle = false;
                if (s_devs[0].online && s_devs[0].addr == addr) h = s_devs[0].handle;
                else if (s_devs[1].online && s_devs[1].addr == addr) h = s_devs[1].handle;
                else {
                    i2c_device_config_t cfg = {
                        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                        .device_address = addr,
                        .scl_speed_hz = I2C_FREQ_HZ,
                    };
                    if (i2c_master_bus_add_device(g_i2c_bus_handle, &cfg, &h) == ESP_OK) {
                        temp_handle = true;
                    }
                }
                if (h) {
                    ina228_read_reg16(h, INA228_REG_MANUF_ID, &manuf);
                    ina228_read_reg16(h, INA228_REG_DEVICE_ID, &dev);
                    if (temp_handle) i2c_master_bus_rm_device(h);
                }
                printf(" - Found 0x%02X (Manuf=0x%04X, DevID=0x%04X)%s\n",
                       addr, manuf, dev, (manuf == 0x5449) ? " [INA228]" : "");
            } else {
                printf(" - Found 0x%02X%s\n", addr,
                       (addr == 0x36) ? " [MAX17048]" : ((addr == 0x68) ? " [RTC]" : ""));
            }
        }
    }
    printf("I2C scan complete (%d device(s) found).\n", found);
}

esp_err_t ina228_read_ch(uint8_t ch, ina228_data_t *data) {
    if (ch >= 2 || !data) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(data, 0, sizeof(*data));

    if (!s_devs[ch].online || !s_devs[ch].handle) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_dev_handle_t handle = s_devs[ch].handle;
    int32_t raw_vbus = 0, raw_vshunt = 0, raw_curr = 0;
    uint32_t raw_power = 0;
    uint16_t raw_temp = 0;
    uint64_t raw_energy = 0;
    int ok_reads = 0;

    if (ina228_read_reg24(handle, INA228_REG_VBUS, &raw_vbus) == ESP_OK) {
        data->bus_voltage_v = (float)(raw_vbus >> 4) * VBUS_LSB_V;
        ok_reads++;
    }

    if (ina228_read_reg24(handle, INA228_REG_VSHUNT, &raw_vshunt) == ESP_OK) {
        data->shunt_voltage_mv = (float)(raw_vshunt >> 4) * (VSHUNT_LSB_V * 1000.0f);
        ok_reads++;
    }

    if (ina228_read_reg24(handle, INA228_REG_CURRENT, &raw_curr) == ESP_OK) {
        data->current_a = (float)(raw_curr >> 4) * s_current_lsb;
        ok_reads++;
    }

    if (ina228_read_reg24_u(handle, INA228_REG_POWER, &raw_power) == ESP_OK) {
        data->power_w = (float)raw_power * (3.2f * s_current_lsb);
        ok_reads++;
    }

    if (ok_reads == 0) {
        s_devs[ch].err_count++;
        if (s_devs[ch].err_count >= 3) {
            ESP_LOGW(TAG, "INA228 #%d (0x%02X) lost communication (3 consecutive failures) -> marking offline",
                     ch + 1, s_devs[ch].addr);
            i2c_master_bus_rm_device(s_devs[ch].handle);
            s_devs[ch].handle = NULL;
            s_devs[ch].online = false;
        }
        return ESP_FAIL;
    }
    s_devs[ch].err_count = 0;

    if (ina228_read_reg40(handle, INA228_REG_ENERGY, &raw_energy) == ESP_OK) {
        double energy_joules = (double)raw_energy * (16.0 * 3.2 * (double)s_current_lsb);
        data->energy_wh = (float)(energy_joules / 3600.0);
    }

    if (ina228_read_reg16(handle, INA228_REG_DIETEMP, &raw_temp) == ESP_OK) {
        data->die_temp_c = (float)((int16_t)raw_temp) * TEMP_LSB_C;
    }

    // Periodically verify SHUNT_CAL in case isolated INA228 browned out and reset registers
    if ((++s_devs[ch].read_count % 10) == 0) {
        uint16_t cal_check = 0;
        if (ina228_read_reg16(handle, INA228_REG_SHUNT_CAL, &cal_check) == ESP_OK && cal_check == 0 && s_devs[ch].cal_reg != 0) {
            ESP_LOGW(TAG, "INA228 #%d (0x%02X) SHUNT_CAL cleared (brownout reset) -> reconfiguring!",
                     ch + 1, s_devs[ch].addr);
            uint16_t adc_cfg = (0x0F << 12) | (0x03 << 9) | (0x03 << 6) | (0x03 << 3) | 0x02;
            ina228_write_reg(handle, INA228_REG_ADC_CONFIG, adc_cfg);
            ina228_write_reg(handle, INA228_REG_SHUNT_CAL, s_devs[ch].cal_reg);
        }
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
