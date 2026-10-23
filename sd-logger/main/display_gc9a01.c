#include <math.h>
#include "display_gc9a01.h"
#include "config_pins.h"
#include "boot_logo.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "GC9A01";

#define LCD_WIDTH   240
#define LCD_HEIGHT  240

// RGB565 Colors
#define C_BLACK         0x0000
#define C_NAVY          0x000F
#define C_DARK_GRAY     0x18E3
#define C_CARD_BG       0x2124
#define C_CYAN          0x07FF
#define C_GREEN         0x07E0
#define C_YELLOW        0xFFE0
#define C_ORANGE        0xFD20
#define C_RED           0xF800
#define C_WHITE         0xFFFF
#define C_GRAY          0x8410

static esp_lcd_panel_handle_t s_panel_handle = NULL;
static uint16_t *s_fb = NULL;

// 5x7 ASCII Bitmap Font
static const uint8_t font5x7[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // 32 ' '
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // 33 '!'
    {0x00, 0x07, 0x00, 0x07, 0x00}, // 34 '"'
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // 35 '#'
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // 36 '$'
    {0x23, 0x13, 0x08, 0x64, 0x62}, // 37 '%'
    {0x36, 0x49, 0x55, 0x22, 0x50}, // 38 '&'
    {0x00, 0x05, 0x03, 0x00, 0x00}, // 39 single quote
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // 40 '('
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // 41 ')'
    {0x14, 0x08, 0x3E, 0x08, 0x14}, // 42 '*'
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // 43 '+'
    {0x00, 0x50, 0x30, 0x00, 0x00}, // 44 ','
    {0x08, 0x08, 0x08, 0x08, 0x08}, // 45 '-'
    {0x00, 0x60, 0x60, 0x00, 0x00}, // 46 '.'
    {0x20, 0x10, 0x08, 0x04, 0x02}, // 47 '/'
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 48 '0'
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 49 '1'
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 50 '2'
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 51 '3'
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 52 '4'
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 53 '5'
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 54 '6'
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 55 '7'
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 56 '8'
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 57 '9'
    {0x00, 0x36, 0x36, 0x00, 0x00}, // 58 ':'
    {0x00, 0x56, 0x36, 0x00, 0x00}, // 59 ';'
    {0x08, 0x14, 0x22, 0x41, 0x00}, // 60 '<'
    {0x14, 0x14, 0x14, 0x14, 0x14}, // 61 '='
    {0x00, 0x41, 0x22, 0x14, 0x08}, // 62 '>'
    {0x02, 0x01, 0x51, 0x09, 0x06}, // 63 '?'
    {0x32, 0x49, 0x79, 0x41, 0x3E}, // 64 '@'
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // 65 'A'
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // 66 'B'
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // 67 'C'
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // 68 'D'
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // 69 'E'
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // 70 'F'
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, // 71 'G'
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // 72 'H'
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // 73 'I'
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // 74 'J'
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // 75 'K'
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // 76 'L'
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // 77 'M'
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // 78 'N'
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // 79 'O'
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // 80 'P'
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // 81 'Q'
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // 82 'R'
    {0x46, 0x49, 0x49, 0x49, 0x31}, // 83 'S'
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // 84 'T'
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // 85 'U'
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // 86 'V'
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, // 87 'W'
    {0x63, 0x14, 0x08, 0x14, 0x63}, // 88 'X'
    {0x07, 0x08, 0x70, 0x08, 0x07}, // 89 'Y'
    {0x61, 0x51, 0x49, 0x45, 0x43}, // 90 'Z'
    {0x00, 0x7F, 0x41, 0x41, 0x00}, // 91 '['
    {0x02, 0x04, 0x08, 0x10, 0x20}, // 92 '\'
    {0x00, 0x41, 0x41, 0x7F, 0x00}, // 93 ']'
    {0x04, 0x02, 0x01, 0x02, 0x04}, // 94 '^'
    {0x40, 0x40, 0x40, 0x40, 0x40}, // 95 '_'
    {0x00, 0x01, 0x02, 0x04, 0x00}, // 96 '`'
    {0x20, 0x54, 0x54, 0x54, 0x78}, // 97 'a'
    {0x7F, 0x48, 0x44, 0x44, 0x38}, // 98 'b'
    {0x38, 0x44, 0x44, 0x44, 0x20}, // 99 'c'
    {0x38, 0x44, 0x44, 0x48, 0x7F}, // 100 'd'
    {0x38, 0x54, 0x54, 0x54, 0x18}, // 101 'e'
    {0x08, 0x7E, 0x09, 0x01, 0x02}, // 102 'f'
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, // 103 'g'
    {0x7F, 0x08, 0x04, 0x04, 0x78}, // 104 'h'
    {0x00, 0x44, 0x7D, 0x40, 0x00}, // 105 'i'
    {0x20, 0x40, 0x44, 0x3D, 0x00}, // 106 'j'
    {0x7F, 0x10, 0x28, 0x44, 0x00}, // 107 'k'
    {0x00, 0x41, 0x7F, 0x40, 0x00}, // 108 'l'
    {0x7C, 0x04, 0x18, 0x04, 0x78}, // 109 'm'
    {0x7C, 0x08, 0x04, 0x04, 0x78}, // 110 'n'
    {0x38, 0x44, 0x44, 0x44, 0x38}, // 111 'o'
    {0x7C, 0x14, 0x14, 0x14, 0x08}, // 112 'p'
    {0x08, 0x14, 0x14, 0x18, 0x7C}, // 113 'q'
    {0x7C, 0x08, 0x04, 0x04, 0x08}, // 114 'r'
    {0x48, 0x54, 0x54, 0x54, 0x20}, // 115 's'
    {0x04, 0x3F, 0x44, 0x40, 0x20}, // 116 't'
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, // 117 'u'
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, // 118 'v'
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, // 119 'w'
    {0x44, 0x28, 0x10, 0x28, 0x44}, // 120 'x'
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, // 121 'y'
    {0x44, 0x64, 0x54, 0x4C, 0x44}, // 122 'z'
    {0x00, 0x08, 0x36, 0x41, 0x00}, // 123 '{'
    {0x00, 0x00, 0x7F, 0x00, 0x00}, // 124 '|'
    {0x00, 0x41, 0x36, 0x08, 0x00}, // 125 '}'
    {0x08, 0x08, 0x2A, 0x1C, 0x08}  // 126 '~'
};

static inline void fb_set_pixel(int x, int y, uint16_t color) {
    if (x >= 0 && x < LCD_WIDTH && y >= 0 && y < LCD_HEIGHT) {
        s_fb[y * LCD_WIDTH + x] = (color >> 8) | (color << 8);
    }
}

static void fb_draw_char(int x, int y, char c, uint16_t color, int scale) {
    if (c < 32 || c > 126) c = ' ';
    const uint8_t *glyph = font5x7[c - 32];

    for (int i = 0; i < 5; i++) {
        uint8_t line = glyph[i];
        for (int j = 0; j < 7; j++) {
            if (line & (1 << j)) {
                if (scale == 1) {
                    fb_set_pixel(x + i, y + j, color);
                } else {
                    for (int sx = 0; sx < scale; sx++) {
                        for (int sy = 0; sy < scale; sy++) {
                            fb_set_pixel(x + i * scale + sx, y + j * scale + sy, color);
                        }
                    }
                }
            }
        }
    }
}

static void fb_draw_string(int x, int y, const char *str, uint16_t color, int scale) {
    int cur_x = x;
    while (*str) {
        fb_draw_char(cur_x, y, *str, color, scale);
        cur_x += (5 * scale) + (1 * scale);
        str++;
    }
}

static void fb_draw_string_centered(int cx, int y, const char *str, uint16_t color, int scale) {
    int len = strlen(str);
    int total_width = len * 6 * scale;
    int start_x = cx - (total_width / 2);
    fb_draw_string(start_x, y, str, color, scale);
}

static void fb_draw_circle_arc(int cx, int cy, int r_out, int r_in, uint16_t color) {
    int r_out_sq = r_out * r_out;
    int r_in_sq = r_in * r_in;

    for (int y = cy - r_out; y <= cy + r_out; y++) {
        if (y < 0 || y >= LCD_HEIGHT) continue;
        int dy = y - cy;
        int dy_sq = dy * dy;

        for (int x = cx - r_out; x <= cx + r_out; x++) {
            if (x < 0 || x >= LCD_WIDTH) continue;
            int dx = x - cx;
            int dist_sq = dx * dx + dy_sq;
            if (dist_sq <= r_out_sq && dist_sq >= r_in_sq) {
                fb_set_pixel(x, y, color);
            }
        }
    }
}

static void fb_draw_rect(int x, int y, int w, int h, uint16_t color) {
    for (int i = x; i < x + w; i++) {
        fb_set_pixel(i, y, color);
        fb_set_pixel(i, y + h - 1, color);
    }
    for (int j = y; j < y + h; j++) {
        fb_set_pixel(x, j, color);
        fb_set_pixel(x + w - 1, j, color);
    }
}

static void fb_fill_rect(int x, int y, int w, int h, uint16_t color) {
    for (int i = x; i < x + w; i++) {
        for (int j = y; j < y + h; j++) {
            fb_set_pixel(i, j, color);
        }
    }
}

esp_err_t display_gc9a01_init(void) {
    ESP_LOGI(TAG, "Initializing GC9A01 LCD Display (SPI2_HOST)...");

    s_fb = (uint16_t *)heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
                                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_fb) {
        s_fb = (uint16_t *)malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));
        if (!s_fb) {
            ESP_LOGE(TAG, "Failed to allocate 240x240 framebuffer");
            return ESP_ERR_NO_MEM;
        }
    }
    memset(s_fb, 0, LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));

    gpio_config_t bk_cfg = {
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&bk_cfg);
    gpio_set_level(PIN_LCD_BL, 1);

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
    };
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = 10 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &s_panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_gc9a01 failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_panel_reset(s_panel_handle);
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_lcd_panel_init(s_panel_handle);
    esp_lcd_panel_invert_color(s_panel_handle, true);
    esp_lcd_panel_mirror(s_panel_handle, true, false);
    esp_lcd_panel_disp_on_off(s_panel_handle, true);

    ESP_LOGI(TAG, "GC9A01 LCD initialized successfully via esp_lcd");
    return ESP_OK;
}

void display_gc9a01_show_boot_logo(uint32_t duration_ms) {
    if (!s_panel_handle) return;
    ESP_LOGI(TAG, "Displaying Wallhamn Boot Logo for %lu ms...", (unsigned long)duration_ms);
    // Push the full 240x240 Wallhamn logo from flash
    esp_lcd_panel_draw_bitmap(s_panel_handle, 0, 0, LCD_WIDTH, LCD_HEIGHT, s_boot_logo);
    gpio_set_level(PIN_LCD_BL, 1);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
}

void display_gc9a01_set_brightness(uint8_t brightness_pct) {
    gpio_set_level(PIN_LCD_BL, brightness_pct > 0 ? 1 : 0);
}

static void render_screen_overview(const system_state_t *state) {
    // Large Center Readout: Total System Current with deadband to eliminate flickering
    float disp_curr = state->latest_telemetry.total_system_current_a;
    char curr_str[32];
    uint16_t curr_color;

    // 50mA Deadband around 0: sits cleanly in calm white instead of jumping between green & red
    if (fabsf(disp_curr) <= 0.05f) {
        snprintf(curr_str, sizeof(curr_str), " 0.00 A");
        curr_color = C_WHITE;
    } else if (disp_curr > 0.05f) {
        snprintf(curr_str, sizeof(curr_str), "%+.2f A", disp_curr);
        curr_color = C_GREEN;
    } else {
        snprintf(curr_str, sizeof(curr_str), "%+.2f A", disp_curr);
        curr_color = C_RED;
    }
    fb_draw_string_centered(120, 80, curr_str, curr_color, 3);

    // Primary Voltage & Total Power
    char vp_str[48];
    float v_display = (state->latest_telemetry.ina1_bus_voltage_v > 0.5f) ?
                       state->latest_telemetry.ina1_bus_voltage_v : state->latest_telemetry.ina2_bus_voltage_v;
    snprintf(vp_str, sizeof(vp_str), "%.2fV  %.1fW", v_display, state->latest_telemetry.total_power_w);
    fb_draw_string_centered(120, 112, vp_str, C_WHITE, 2);

    // Dual INA Currents line
    char ina_str[48];
    snprintf(ina_str, sizeof(ina_str), "I1:%+.2fA  I2:%+.2fA",
             state->latest_telemetry.ina1_current_a,
             state->latest_telemetry.ina2_current_a);
    fb_draw_string_centered(120, 134, ina_str, C_CYAN, 1);

    // LEM Hall Clamps line
    char lem_str[48];
    snprintf(lem_str, sizeof(lem_str), "DHAB:%+.1fA HO:%+.1fA",
             state->latest_telemetry.dhab_current_a,
             state->latest_telemetry.ho100_current_a);
    fb_draw_string_centered(120, 150, lem_str, C_YELLOW, 1);

    // MicroSD Status
    char sd_str[48];
    if (state->sd_mounted) {
        snprintf(sd_str, sizeof(sd_str), "SD:OK  RECS:%lu", (unsigned long)state->records_written);
    } else {
        snprintf(sd_str, sizeof(sd_str), "SD:NO CARD / ERR");
    }
    fb_draw_string_centered(120, 166, sd_str, state->sd_mounted ? C_GREEN : C_ORANGE, 1);
}

static void render_screen_ina(const system_state_t *state) {
    char buf[48];

    fb_fill_rect(36, 70, 168, 48, C_CARD_BG);
    snprintf(buf, sizeof(buf), "INA1 (0x40): %s", state->ina1_online ? "ONLINE" : "OFFLINE");
    fb_draw_string(42, 74, buf, state->ina1_online ? C_GREEN : C_RED, 1);
    snprintf(buf, sizeof(buf), "V: %.3f V", state->latest_telemetry.ina1_bus_voltage_v);
    fb_draw_string(42, 86, buf, C_WHITE, 1);
    snprintf(buf, sizeof(buf), "I: %+.3f A  P: %.2fW",
             state->latest_telemetry.ina1_current_a,
             state->latest_telemetry.ina1_power_w);
    fb_draw_string(42, 98, buf, C_CYAN, 1);
    snprintf(buf, sizeof(buf), "T: %.1f C  Sh: %.2fmV",
             state->latest_telemetry.ina1_die_temp_c,
             state->latest_telemetry.ina1_shunt_mv);
    fb_draw_string(42, 108, buf, C_GRAY, 1);

    fb_fill_rect(36, 126, 168, 48, C_CARD_BG);
    snprintf(buf, sizeof(buf), "INA2 (0x41): %s", state->ina2_online ? "ONLINE" : "OFFLINE");
    fb_draw_string(42, 130, buf, state->ina2_online ? C_GREEN : C_RED, 1);
    snprintf(buf, sizeof(buf), "V: %.3f V", state->latest_telemetry.ina2_bus_voltage_v);
    fb_draw_string(42, 142, buf, C_WHITE, 1);
    snprintf(buf, sizeof(buf), "I: %+.3f A  P: %.2fW",
             state->latest_telemetry.ina2_current_a,
             state->latest_telemetry.ina2_power_w);
    fb_draw_string(42, 154, buf, C_CYAN, 1);
    snprintf(buf, sizeof(buf), "T: %.1f C  Sh: %.2fmV",
             state->latest_telemetry.ina2_die_temp_c,
             state->latest_telemetry.ina2_shunt_mv);
    fb_draw_string(42, 164, buf, C_GRAY, 1);
}

static void render_screen_lem(const system_state_t *state) {
    char buf[48];

    fb_fill_rect(36, 70, 168, 50, C_CARD_BG);
    fb_draw_string(42, 74, "LEM DHAB S/157:", C_YELLOW, 1);
    snprintf(buf, sizeof(buf), "Curr: %+.2f A", state->latest_telemetry.dhab_current_a);
    fb_draw_string(42, 86, buf, C_WHITE, 1);
    snprintf(buf, sizeof(buf), "Range: %s", state->latest_telemetry.dhab_in_high_range ? "+/-500A" : "+/-50A");
    fb_draw_string(42, 98, buf, C_CYAN, 1);
    snprintf(buf, sizeof(buf), "Ch1:%.2fV Ch2:%.2fV",
             state->latest_telemetry.dhab_ch1_50a_raw_v,
             state->latest_telemetry.dhab_ch2_500a_raw_v);
    fb_draw_string(42, 108, buf, C_GRAY, 1);

    fb_fill_rect(36, 128, 168, 46, C_CARD_BG);
    fb_draw_string(42, 132, "LEM HO-100-S (A5):", C_YELLOW, 1);
    snprintf(buf, sizeof(buf), "Curr: %+.2f A", state->latest_telemetry.ho100_current_a);
    fb_draw_string(42, 144, buf, C_WHITE, 1);
    snprintf(buf, sizeof(buf), "Raw ADC: %.3f V", state->latest_telemetry.ho100_raw_v);
    fb_draw_string(42, 156, buf, C_GRAY, 1);
}

static void render_screen_system(const system_state_t *state) {
    char buf[48];

    fb_fill_rect(36, 70, 168, 104, C_CARD_BG);
    fb_draw_string_centered(120, 74, "SYSTEM STATUS", C_YELLOW, 1);

    if (state->latest_telemetry.battery_v >= 2.50f) {
        snprintf(buf, sizeof(buf), "18650: %.2fV (%u%%)",
                 state->latest_telemetry.battery_v, state->latest_telemetry.battery_pct);
        fb_draw_string(42, 90, buf, state->latest_telemetry.battery_pct > 20 ? C_GREEN : C_RED, 1);
    } else {
        fb_draw_string(42, 90, "18650: USB POWER", C_CYAN, 1);
    }

    snprintf(buf, sizeof(buf), "SD: %s", state->sd_mounted ? "MOUNTED (FAT32)" : "NOT MOUNTED");
    fb_draw_string(42, 104, buf, state->sd_mounted ? C_GREEN : C_ORANGE, 1);

    snprintf(buf, sizeof(buf), "Recs: %lu", (unsigned long)state->records_written);
    fb_draw_string(42, 118, buf, C_WHITE, 1);

    snprintf(buf, sizeof(buf), "Drops: %lu", (unsigned long)state->queue_overflow_count);
    fb_draw_string(42, 132, buf, state->queue_overflow_count == 0 ? C_GREEN : C_RED, 1);

    snprintf(buf, sizeof(buf), "Mode: %s", state->active_mode == LOG_MODE_TIMED ? "TIMED" : "UNTIL DEAD");
    fb_draw_string(42, 146, buf, C_CYAN, 1);

    if (state->is_logging) {
        snprintf(buf, sizeof(buf), "Time: %lus", (unsigned long)state->elapsed_sec);
        fb_draw_string(42, 158, buf, C_WHITE, 1);
    }
}

void display_gc9a01_update(const system_state_t *state) {
    if (!s_panel_handle || !s_fb || !state) return;

    memset(s_fb, 0, LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));

    uint16_t dial_color = state->is_logging ? C_RED : C_CYAN;
    fb_draw_circle_arc(120, 120, 118, 115, dial_color);

    fb_draw_string_centered(120, 18, "VAM LOGGER", C_YELLOW, 2);

    // Battery Bank Indicator (Y = 34): Cleanly placed under title, never overlapping page info!
    float vbat = state->latest_telemetry.battery_v;
    uint8_t pct = state->latest_telemetry.battery_pct;
    if (vbat >= 2.50f) {
        int bx = 76, by = 33;
        fb_draw_rect(bx, by, 20, 10, C_WHITE);
        fb_fill_rect(bx + 20, by + 2, 2, 6, C_WHITE); // Positive nub

        int fill_w = (pct * 16) / 100;
        if (fill_w > 16) fill_w = 16;
        uint16_t fill_color = (pct > 50) ? C_GREEN : ((pct >= 20) ? C_YELLOW : C_RED);
        if (fill_w > 0) {
            fb_fill_rect(bx + 2, by + 2, fill_w, 6, fill_color);
        }

        char batt_str[32];
        snprintf(batt_str, sizeof(batt_str), "%.2fV %u%%", vbat, pct);
        fb_draw_string(bx + 28, by + 1, batt_str, C_WHITE, 1);
    } else {
        fb_draw_string_centered(120, 34, "[USB POWER]", C_CYAN, 1);
    }

    // Recording / Standby Badge (Y = 50)
    if (state->is_logging) {
        fb_fill_rect(65, 48, 110, 16, C_RED);
        fb_draw_string_centered(120, 52, state->is_zeroed ? "REC [TARE]" : "RECORDING", C_WHITE, 1);
    } else {
        fb_fill_rect(70, 48, 100, 16, C_CARD_BG);
        fb_draw_string_centered(120, 52, state->is_zeroed ? "STANDBY [TARE]" : "STANDBY", state->is_zeroed ? C_CYAN : C_GREEN, 1);
    }

    switch (state->ui_screen) {
        case 1:
            render_screen_ina(state);
            break;
        case 2:
            render_screen_lem(state);
            break;
        case 3:
            render_screen_system(state);
            break;
        case 0:
        default:
            render_screen_overview(state);
            break;
    }

    // Zero Popup Alert if active
    if (state->zero_popup_timer > 0) {
        fb_fill_rect(40, 95, 160, 36, C_NAVY);
        fb_draw_string_centered(120, 102, state->is_zeroed ? "ZERO / TARE SET!" : "ZERO CLEARED!", C_YELLOW, 1);
        fb_draw_string_centered(120, 114, state->is_zeroed ? "Offsets calibrated" : "Raw values restored", C_WHITE, 1);
    }

    char nav_str[48];
    snprintf(nav_str, sizeof(nav_str), "< P%u/3 > [Hold A1:Zero]", state->ui_screen);
    fb_draw_string_centered(120, 186, nav_str, C_GRAY, 1);

    if (state->is_logging) {
        fb_draw_string_centered(120, 204, "[A2] STOP LOG", C_RED, 1);
    } else {
        fb_draw_string_centered(120, 204, "[A2] START LOG", C_GREEN, 1);
    }

    esp_lcd_panel_draw_bitmap(s_panel_handle, 0, 0, LCD_WIDTH, LCD_HEIGHT, s_fb);
}
