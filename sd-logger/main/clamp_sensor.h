#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // DHAB S/157 Dual-Range
    float dhab_current_a;       // Auto-selected best range
    float dhab_ch1_low_a;       // Channel 1: High Sensitivity (+/-50A nominal)
    float dhab_ch2_high_a;      // Channel 2: High Current (+/-500A nominal)
    float dhab_ch1_v;
    float dhab_ch2_v;
    bool  dhab_high_range;

    // HO-100-S
    float ho100_current_a;
    float ho100_v;
} lem_sensors_data_t;

esp_err_t lem_sensors_init(void);
esp_err_t lem_sensors_read_all(lem_sensors_data_t *data);
esp_err_t lem_sensors_calibrate_zeros(void);

#ifdef __cplusplus
}
#endif
