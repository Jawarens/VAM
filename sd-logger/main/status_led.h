#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_STATE_IDLE = 0,
    LED_STATE_LOGGING,
    LED_STATE_WIFI_ACTIVE,
    LED_STATE_ZERO_CALIBRATED,
    LED_STATE_LOW_BATT,
} led_status_state_t;

esp_err_t status_led_init(void);
void status_led_set_state(led_status_state_t state);
void status_led_update_tick(void);

#ifdef __cplusplus
}
#endif

