#pragma once

#include "esp_err.h"
#include "config_pins.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t display_st7920_init(void);
void display_st7920_show_boot_logo(uint32_t duration_ms);
void display_st7920_set_brightness(uint8_t brightness_pct);
void display_st7920_update(const system_state_t *state);

#ifdef __cplusplus
}
#endif
