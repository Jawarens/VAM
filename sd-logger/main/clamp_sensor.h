#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // DHAB S/157
    float dhab_current_a;
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
