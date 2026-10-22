#include "ds3231.h"
#include "config_pins.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include <sys/time.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "DS3231";
static i2c_master_dev_handle_t s_ds3231_handle = NULL;
extern i2c_master_bus_handle_t g_i2c_bus_handle;

static uint8_t bcd2bin(uint8_t val) {
    return val - 6 * (val >> 4);
}

static uint8_t bin2bcd(uint8_t val) {
    return val + 6 * (val / 10);
}

// Synchronize RTC to build timestamp
static void ds3231_sync_from_build_time(void) {
    const char *date_str = __DATE__; // "Mmm dd yyyy"
    const char *time_str = __TIME__; // "hh:mm:ss"
    
    char mon_str[4];
    int day = 1, year = 2026, hour = 0, min = 0, sec = 0;
    sscanf(date_str, "%3s %d %d", mon_str, &day, &year);
    sscanf(time_str, "%d:%d:%d", &hour, &min, &sec);
    
    const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int mon = 9; // Default October
    for (int i = 0; i < 12; i++) {
        if (strncmp(mon_str, months[i], 3) == 0) {
            mon = i;
            break;
        }
    }
    
    struct tm ti = {
        .tm_year = year - 1900,
        .tm_mon  = mon,
        .tm_mday = day,
        .tm_hour = hour,
        .tm_min  = min,
        .tm_sec  = sec,
        .tm_isdst = -1
    };
    
    ds3231_set_time(&ti);
    ESP_LOGI(TAG, "RTC synchronized to build date: %04d-%02d-%02d %02d:%02d:%02d",
             year, mon + 1, day, hour, min, sec);
}

esp_err_t ds3231_init(void) {
    if (!g_i2c_bus_handle) {
        ESP_LOGE(TAG, "I2C master bus handle is NULL");
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DS3231_I2C_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };

    esp_err_t ret = i2c_master_bus_add_device(g_i2c_bus_handle, &dev_cfg, &s_ds3231_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add DS3231 device to I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }

    struct tm ti;
    ret = ds3231_get_time(&ti);
    if (ret == ESP_OK) {
        if (ti.tm_year < 126) { // Earlier than 2026 -> uninitialized / battery loss
            ESP_LOGW(TAG, "RTC date is prior to 2026 (%04d-%02d-%02d), syncing to build timestamp...",
                     ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
            ds3231_sync_from_build_time();
            ds3231_get_time(&ti);
        }
        ESP_LOGI(TAG, "DS3231 online. Current RTC time: %04d-%02d-%02d %02d:%02d:%02d",
                 ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, ti.tm_hour, ti.tm_min, ti.tm_sec);
        ds3231_sync_system_time();
    }
    return ret;
}

esp_err_t ds3231_get_time(struct tm *timeinfo) {
    if (!s_ds3231_handle || !timeinfo) return ESP_ERR_INVALID_STATE;

    uint8_t reg = DS3231_REG_SECONDS;
    uint8_t data[7];
    esp_err_t err = i2c_master_transmit_receive(s_ds3231_handle, &reg, 1, data, 7, 50);
    if (err != ESP_OK) return err;

    timeinfo->tm_sec  = bcd2bin(data[0] & 0x7F);
    timeinfo->tm_min  = bcd2bin(data[1] & 0x7F);
    timeinfo->tm_hour = bcd2bin(data[2] & 0x3F);
    timeinfo->tm_wday = bcd2bin(data[3] & 0x07) - 1;
    timeinfo->tm_mday = bcd2bin(data[4] & 0x3F);
    timeinfo->tm_mon  = bcd2bin(data[5] & 0x1F) - 1;
    timeinfo->tm_year = bcd2bin(data[6]) + 100; // 2000-based -> 1900-based
    timeinfo->tm_isdst = -1;

    return ESP_OK;
}

esp_err_t ds3231_set_time(const struct tm *timeinfo) {
    if (!s_ds3231_handle || !timeinfo) return ESP_ERR_INVALID_STATE;

    uint8_t data[8];
    data[0] = DS3231_REG_SECONDS;
    data[1] = bin2bcd(timeinfo->tm_sec);
    data[2] = bin2bcd(timeinfo->tm_min);
    data[3] = bin2bcd(timeinfo->tm_hour);
    data[4] = bin2bcd(timeinfo->tm_wday + 1);
    data[5] = bin2bcd(timeinfo->tm_mday);
    data[6] = bin2bcd(timeinfo->tm_mon + 1);
    data[7] = bin2bcd(timeinfo->tm_year >= 100 ? timeinfo->tm_year - 100 : timeinfo->tm_year);

    esp_err_t err = i2c_master_transmit(s_ds3231_handle, data, 8, 50);
    if (err == ESP_OK) {
        ds3231_sync_system_time();
    }
    return err;
}

esp_err_t ds3231_get_temperature(float *temp_c) {
    if (!s_ds3231_handle || !temp_c) return ESP_ERR_INVALID_STATE;

    uint8_t reg = DS3231_REG_TEMP_MSB;
    uint8_t data[2];
    esp_err_t err = i2c_master_transmit_receive(s_ds3231_handle, &reg, 1, data, 2, 50);
    if (err == ESP_OK) {
        int8_t msb = (int8_t)data[0];
        uint8_t lsb = data[1] >> 6;
        *temp_c = (float)msb + ((float)lsb * 0.25f);
    }
    return err;
}

esp_err_t ds3231_sync_system_time(void) {
    struct tm ti;
    esp_err_t err = ds3231_get_time(&ti);
    if (err == ESP_OK) {
        time_t t = mktime(&ti);
        struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
        settimeofday(&tv, NULL);
    }
    return err;
}

void ds3231_format_iso_timestamp(char *dest_buf, size_t max_len) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm ti;
    localtime_r(&tv.tv_sec, &ti);
    snprintf(dest_buf, max_len, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ti.tm_hour, ti.tm_min, ti.tm_sec,
             (long)(tv.tv_usec / 1000));
}
