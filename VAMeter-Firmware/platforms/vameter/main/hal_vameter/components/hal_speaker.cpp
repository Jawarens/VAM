/*
* SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
*
* SPDX-License-Identifier: MIT
*/
#include "../hal_vameter.h"
#include "../hal_config.h"
#include <mooncake.h>
#include <Arduino.h>
#include "driver/ledc.h"
#include "esp_timer.h"

#define BUZZER_LEDC_TIMER       LEDC_TIMER_1
#define BUZZER_LEDC_MODE        LEDC_LOW_SPEED_MODE
#define BUZZER_LEDC_CHANNEL     LEDC_CHANNEL_1
#define BUZZER_LEDC_DUTY_RES    LEDC_TIMER_10_BIT

static esp_timer_handle_t s_beep_timer = nullptr;

static void _beep_timer_callback(void* arg)
{
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

void HAL_VAMeter::_seaker_init()
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = BUZZER_LEDC_MODE,
        .duty_resolution  = BUZZER_LEDC_DUTY_RES,
        .timer_num        = BUZZER_LEDC_TIMER,
        .freq_hz          = 2700,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .gpio_num       = HAL_PIN_BUZZ,
        .speed_mode     = BUZZER_LEDC_MODE,
        .channel        = BUZZER_LEDC_CHANNEL,
        .intr_type      = LEDC_INTR_DISABLE,
        .timer_sel      = BUZZER_LEDC_TIMER,
        .duty           = 0,
        .hpoint         = 0
    };
    ledc_channel_config(&ledc_channel);

    esp_timer_create_args_t timer_args = {
        .callback = &_beep_timer_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "buzzer_timer"
    };
    esp_timer_create(&timer_args, &s_beep_timer);

    if (_config.beepOn)
    {
        beep(3200, 50);
        delay(80);
        beep(3200, 50);
    }
}

void HAL_VAMeter::beep(float frequency, uint32_t duration)
{
    if (!_config.beepOn) return;
    if (frequency <= 0) return;

    if (s_beep_timer) {
        esp_timer_stop(s_beep_timer);
    }

    ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, (uint32_t)frequency);
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 512); // 50% duty
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);

    if (duration != 4294967295U && duration > 0)
    {
        if (s_beep_timer) {
            esp_timer_start_once(s_beep_timer, (uint64_t)duration * 1000);
        }
    }
}

void HAL_VAMeter::beepStop()
{
    if (s_beep_timer) {
        esp_timer_stop(s_beep_timer);
    }
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}
