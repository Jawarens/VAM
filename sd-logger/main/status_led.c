#include "status_led.h"
#include "config_pins.h"
#include "led_strip.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "STATUS_LED";
static led_strip_handle_t s_led_strip = NULL;
static led_status_state_t s_current_state = LED_STATE_IDLE;

esp_err_t status_led_init(void) {
    // 1. Enable power gate to NeoPixel (GPIO 21 on Feather S3)
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = (1ULL << PIN_NEOPIXEL_PWR),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr_cfg);
    gpio_set_level(PIN_NEOPIXEL_PWR, 1);

    led_strip_config_t strip_config = {
        .strip_gpio_num = PIN_RGB_LED, // GPIO 33
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        }
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags = {
            .with_dma = false,
        }
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip);
    if (ret == ESP_OK && s_led_strip) {
        led_strip_clear(s_led_strip);
        ESP_LOGI(TAG, "Onboard NeoPixel initialized on GPIO %d (Pwr: GPIO %d)", PIN_RGB_LED, PIN_NEOPIXEL_PWR);
    } else {
        ESP_LOGW(TAG, "NeoPixel init failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

void status_led_set_state(led_status_state_t state) {
    s_current_state = state;
}

void status_led_update_tick(void) {
    if (!s_led_strip) return;

    uint32_t ms = (esp_timer_get_time() / 1000) % 1000;

    switch (s_current_state) {
        case LED_STATE_LOGGING: {
            // Smooth breathing Red pulse
            uint8_t brightness = (ms < 500) ? (ms * 32 / 500) : ((1000 - ms) * 32 / 500);
            if (brightness < 4) brightness = 4;
            led_strip_set_pixel(s_led_strip, 0, brightness, 0, 0); // Red
            break;
        }
        case LED_STATE_WIFI_ACTIVE:
            // Solid Electric Cyan
            led_strip_set_pixel(s_led_strip, 0, 0, 20, 25);
            break;

        case LED_STATE_ZERO_CALIBRATED:
            // Vivid Green
            led_strip_set_pixel(s_led_strip, 0, 0, 30, 0);
            break;

        case LED_STATE_LOW_BATT:
            // Flashing Amber
            if (ms < 500) {
                led_strip_set_pixel(s_led_strip, 0, 25, 12, 0); // Amber
            } else {
                led_strip_clear(s_led_strip);
            }
            break;

        case LED_STATE_IDLE:
        default:
            // Soft calm blue heartbeat (dim)
            led_strip_set_pixel(s_led_strip, 0, 0, 2, 8);
            break;
    }

    led_strip_refresh(s_led_strip);
}

