#pragma once

#include "esp_err.h"
#include "config_pins.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_MOUNT_POINT "/sdcard"

esp_err_t sd_logger_init(void);
esp_err_t sd_logger_start_session(void);
esp_err_t sd_logger_stop_session(void);
void sd_logger_diag(void);
esp_err_t sd_logger_push_record(const telemetry_record_t *record);
bool      sd_logger_is_ready(void);

#ifdef __cplusplus
}
#endif
