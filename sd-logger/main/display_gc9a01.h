#pragma once

#include "config_pins.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t display_gc9a01_init(void);
void display_gc9a01_show_boot_logo(uint32_t duration_ms);
void display_gc9a01_update(const system_state_t *state);
void display_gc9a01_set_brightness(uint8_t brightness_pct);

#ifdef __cplusplus
}
#endif
