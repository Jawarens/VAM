#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float battery_voltage_v;
    uint8_t state_of_charge_pct;
    bool is_charging;
    bool is_critical_low;
} battery_status_t;

esp_err_t power_mgr_init(void);
esp_err_t power_mgr_read_battery(battery_status_t *status);
void      power_mgr_set_cpu_freq_low_power(bool enable_low_power);

#ifdef __cplusplus
}
#endif
