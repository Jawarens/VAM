#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS3231_I2C_ADDR         0x68

// DS3231 Register Map
#define DS3231_REG_SECONDS      0x00
#define DS3231_REG_MINUTES      0x01
#define DS3231_REG_HOURS        0x02
#define DS3231_REG_DAY          0x03
#define DS3231_REG_DATE         0x04
#define DS3231_REG_MONTH        0x05
#define DS3231_REG_YEAR         0x06
#define DS3231_REG_CONTROL      0x0E
#define DS3231_REG_STATUS       0x0F
#define DS3231_REG_TEMP_MSB     0x11
#define DS3231_REG_TEMP_LSB     0x12

esp_err_t ds3231_init(void);
esp_err_t ds3231_get_time(struct tm *timeinfo);
esp_err_t ds3231_set_time(const struct tm *timeinfo);
esp_err_t ds3231_get_temperature(float *temp_c);
esp_err_t ds3231_sync_system_time(void);
void      ds3231_format_iso_timestamp(char *dest_buf, size_t max_len);

#ifdef __cplusplus
}
#endif
