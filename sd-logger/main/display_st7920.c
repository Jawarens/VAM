#include "display_st7920.h"
#include "config_pins.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "ST7920";

#define LCD_W 128
#define LCD_H 64
#define FB_BYTES (LCD_W * LCD_H / 8)

static spi_device_handle_t s_spi_dev = NULL;
static uint8_t s_fb[FB_BYTES];
static uint8_t s_prev_fb[FB_BYTES];
static bool s_force_full_redraw = true;

// 5x7 ASCII Bitmap Font (characters 32..126)
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

static inline void st7920_encode_byte(uint8_t *dst, bool is_data, uint8_t val) {
    dst[0] = is_data ? 0xFA : 0xF8;
    dst[1] = (uint8_t)(val & 0xF0);
    dst[2] = (uint8_t)((val << 4) & 0xF0);
}

static void st7920_send_byte(bool is_data, uint8_t val) {
    if (!s_spi_dev) return;
    uint8_t pkt[3];
    st7920_encode_byte(pkt, is_data, val);

    spi_transaction_t t = {
        .length = 24,
        .tx_buffer = pkt,
    };

    gpio_set_level(PIN_ST7920_CS, 1);
    esp_rom_delay_us(2);
    spi_device_polling_transmit(s_spi_dev, &t);
    esp_rom_delay_us(2);
    gpio_set_level(PIN_ST7920_CS, 0);
    esp_rom_delay_us(20);
}

static inline void st7920_cmd(uint8_t cmd) {
    st7920_send_byte(false, cmd);
}

static void st7920_flush_row(uint8_t row) {
    if (!s_spi_dev || row >= 32) return;

    // 2 address commands + 16 top bytes + 16 bottom bytes = 34 logical bytes -> 102 SPI bytes
    uint8_t tx_buf[34 * 3];
    int idx = 0;

    st7920_encode_byte(&tx_buf[idx], false, (uint8_t)(0x80 | row));
    idx += 3;
    st7920_encode_byte(&tx_buf[idx], false, 0x80);
    idx += 3;

    const uint8_t *top = &s_fb[row * 16];
    for (int i = 0; i < 16; i++) {
        st7920_encode_byte(&tx_buf[idx], true, top[i]);
        idx += 3;
    }

    const uint8_t *bot = &s_fb[(row + 32) * 16];
    for (int i = 0; i < 16; i++) {
        st7920_encode_byte(&tx_buf[idx], true, bot[i]);
        idx += 3;
    }

    spi_transaction_t t = {
        .length = sizeof(tx_buf) * 8,
        .tx_buffer = tx_buf,
    };

    gpio_set_level(PIN_ST7920_CS, 1);
    esp_rom_delay_us(3);
    spi_device_polling_transmit(s_spi_dev, &t);
    esp_rom_delay_us(3);
    gpio_set_level(PIN_ST7920_CS, 0);
    esp_rom_delay_us(20);
}

static void st7920_flush_dirty(void) {
    for (uint8_t row = 0; row < 32; row++) {
        size_t top_off = (size_t)row * 16;
        size_t bot_off = (size_t)(row + 32) * 16;
        if (s_force_full_redraw ||
            memcmp(&s_fb[top_off], &s_prev_fb[top_off], 16) != 0 ||
            memcmp(&s_fb[bot_off], &s_prev_fb[bot_off], 16) != 0) {
            st7920_flush_row(row);
        }
    }
    memcpy(s_prev_fb, s_fb, FB_BYTES);
    s_force_full_redraw = false;
}

static inline void fb_clear(void) {
    memset(s_fb, 0, FB_BYTES);
}

static inline void fb_set_pixel(int x, int y, bool on) {
    if (x < 0 || x >= LCD_W || y < 0 || y >= LCD_H) return;
    size_t byte_idx = (size_t)y * 16 + (size_t)(x >> 3);
    uint8_t mask = (uint8_t)(0x80 >> (x & 7));
    if (on) {
        s_fb[byte_idx] |= mask;
    } else {
        s_fb[byte_idx] &= (uint8_t)(~mask);
    }
}

static void fb_fill_rect(int x, int y, int w, int h, bool on) {
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            fb_set_pixel(xx, yy, on);
        }
    }
}

static void fb_draw_rect(int x, int y, int w, int h, bool on) {
    for (int xx = x; xx < x + w; xx++) {
        fb_set_pixel(xx, y, on);
        fb_set_pixel(xx, y + h - 1, on);
    }
    for (int yy = y; yy < y + h; yy++) {
        fb_set_pixel(x, yy, on);
        fb_set_pixel(x + w - 1, yy, on);
    }
}

static void fb_draw_hline(int x, int y, int w, bool on) {
    for (int xx = x; xx < x + w; xx++) {
        fb_set_pixel(xx, y, on);
    }
}

static void fb_draw_char(int x, int y, char c, int scale, bool on) {
    if (c < 32 || c > 126) c = ' ';
    const uint8_t *glyph = font5x7[c - 32];
    for (int i = 0; i < 5; i++) {
        uint8_t col = glyph[i];
        for (int j = 0; j < 7; j++) {
            if (col & (1 << j)) {
                if (scale == 1) {
                    fb_set_pixel(x + i, y + j, on);
                } else {
                    fb_fill_rect(x + i * scale, y + j * scale, scale, scale, on);
                }
            }
        }
    }
}

static void fb_draw_string(int x, int y, const char *str, int scale, bool on) {
    int cx = x;
    while (*str) {
        fb_draw_char(cx, y, *str, scale, on);
        cx += 6 * scale;
        str++;
    }
}

static void fb_draw_string_centered(int y, const char *str, int scale, bool on) {
    int len = (int)strlen(str);
    int total_w = len * 6 * scale - scale;
    int start_x = (LCD_W - total_w) / 2;
    if (start_x < 0) start_x = 0;
    fb_draw_string(start_x, y, str, scale, on);
}

esp_err_t display_st7920_init(void) {
    ESP_LOGI(TAG, "Initializing 12864B V2.0 (ST7920 128x64) on dedicated SPI3_HOST...");

    gpio_config_t cs_cfg = {
        .pin_bit_mask = (1ULL << PIN_ST7920_CS) | (1ULL << PIN_ST7920_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cs_cfg);
    gpio_set_level(PIN_ST7920_CS, 0); // Active HIGH CS -> idle LOW
    gpio_set_level(PIN_ST7920_BL, 0); // Backlight off until first draw

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_ST7920_CLK,
        .mosi_io_num = PIN_ST7920_SID,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 256,
    };
    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to initialize SPI3_HOST: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 250000, // 250 kHz -> 96 us per 3-byte frame (>72 us ST7920 execution time)
        .mode = 0,
        .spics_io_num = -1,       // Manual active-HIGH CS
        .queue_size = 1,
    };
    ret = spi_bus_add_device(SPI3_HOST, &devcfg, &s_spi_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ST7920 SPI device: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(50));
    st7920_cmd(0x30); // 8-bit basic instruction set
    esp_rom_delay_us(200);
    st7920_cmd(0x30);
    esp_rom_delay_us(200);
    st7920_cmd(0x0C); // Display ON, cursor OFF
    esp_rom_delay_us(200);
    st7920_cmd(0x01); // Clear DDRAM
    vTaskDelay(pdMS_TO_TICKS(10));
    st7920_cmd(0x06); // Entry mode increment
    esp_rom_delay_us(200);
    st7920_cmd(0x34); // Extended instruction set (graphic OFF)
    esp_rom_delay_us(200);

    fb_clear();
    s_force_full_redraw = true;
    st7920_flush_dirty();

    st7920_cmd(0x36); // Extended instruction set + Graphic display ON
    esp_rom_delay_us(200);

    ESP_LOGI(TAG, "ST7920 12864B V2.0 initialized (CS=GPIO%d, SID=GPIO%d, CLK=GPIO%d)",
             PIN_ST7920_CS, PIN_ST7920_SID, PIN_ST7920_CLK);
    return ESP_OK;
}

void display_st7920_show_boot_logo(uint32_t duration_ms) {
    if (!s_spi_dev) return;
    fb_clear();
    fb_draw_rect(0, 0, LCD_W, LCD_H, true);
    fb_draw_rect(2, 2, LCD_W - 4, LCD_H - 4, true);
    fb_draw_string_centered(10, "VAM LOGGER", 2, true);
    fb_draw_hline(8, 28, LCD_W - 16, true);
    fb_draw_string_centered(34, "WALLHAMN AB", 1, true);
    fb_draw_string_centered(48, "12864B ST7920", 1, true);

    s_force_full_redraw = true;
    st7920_flush_dirty();

    gpio_set_level(PIN_ST7920_BL, 1);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
}

void display_st7920_set_brightness(uint8_t brightness_pct) {
    gpio_set_level(PIN_ST7920_BL, brightness_pct > 0 ? 1 : 0);
}

static void render_top_bar(const system_state_t *state) {
    fb_fill_rect(0, 0, LCD_W, 9, true);

    const char *mode_txt;
    if (state->is_logging) {
        mode_txt = state->is_zeroed ? "REC*" : "REC";
    } else if (state->wifi_portal_active) {
        mode_txt = "WIFI";
    } else {
        mode_txt = state->is_zeroed ? "STB*" : "STBY";
    }
    fb_draw_string(2, 1, mode_txt, 1, false);

    char page_txt[16];
    snprintf(page_txt, sizeof(page_txt), "P%u/4", state->ui_screen);
    fb_draw_string(46, 1, page_txt, 1, false);

    char batt_txt[24];
    float vbat = state->latest_telemetry.battery_v;
    if (state->battery_present && vbat >= 2.50f) {
        snprintf(batt_txt, sizeof(batt_txt), "%.1fV %u%%", vbat, state->latest_telemetry.battery_pct);
    } else {
        snprintf(batt_txt, sizeof(batt_txt), "USB");
    }
    int bw = (int)strlen(batt_txt) * 6;
    fb_draw_string(LCD_W - bw - 1, 1, batt_txt, 1, false);
}

static void render_overview(const system_state_t *state) {
    char buf[64];
    float disp_curr = state->latest_telemetry.total_system_current_a;

    if (fabsf(disp_curr) <= 0.05f) {
        snprintf(buf, sizeof(buf), "0.00 A");
    } else {
        snprintf(buf, sizeof(buf), "%+.2f A", disp_curr);
    }
    fb_draw_string_centered(13, buf, 2, true);

    float v_disp = (state->latest_telemetry.ina1_bus_voltage_v > 0.5f)
                       ? state->latest_telemetry.ina1_bus_voltage_v
                       : state->latest_telemetry.ina2_bus_voltage_v;
    snprintf(buf, sizeof(buf), "%.2fV   %.1fW", v_disp, state->latest_telemetry.total_power_w);
    fb_draw_string_centered(31, buf, 1, true);

    snprintf(buf, sizeof(buf), "I1:%+.2f I2:%+.2f",
             state->latest_telemetry.ina1_current_a,
             state->latest_telemetry.ina2_current_a);
    fb_draw_string_centered(42, buf, 1, true);

    if (state->sd_mounted) {
        snprintf(buf, sizeof(buf), "DH-%c%s:%+.1f HO%s:%+.1f",
                 state->latest_telemetry.dhab_in_high_range ? 'H' : 'L',
                 state->dhab_invert ? "*" : "",
                 state->latest_telemetry.dhab_current_a,
                 state->ho100_invert ? "*" : "",
                 state->latest_telemetry.ho100_current_a);
    } else {
        snprintf(buf, sizeof(buf), "NO SD! D:%+.1f H:%+.1f",
                 state->latest_telemetry.dhab_current_a,
                 state->latest_telemetry.ho100_current_a);
    }
    fb_draw_string_centered(53, buf, 1, true);
}

static void render_ina(const system_state_t *state) {
    char buf[64];

    snprintf(buf, sizeof(buf), "INA1(%02X): %s",
             state->ina1_addr, state->ina1_online ? "ONLINE" : "OFFLINE");
    fb_draw_string(2, 12, buf, 1, true);

    snprintf(buf, sizeof(buf), "%.2fV %+.3fA %.1fW",
             state->latest_telemetry.ina1_bus_voltage_v,
             state->latest_telemetry.ina1_current_a,
             state->latest_telemetry.ina1_power_w);
    fb_draw_string(2, 22, buf, 1, true);

    fb_draw_hline(0, 33, LCD_W, true);

    snprintf(buf, sizeof(buf), "INA2(%02X): %s",
             state->ina2_addr, state->ina2_online ? "ONLINE" : "OFFLINE");
    fb_draw_string(2, 36, buf, 1, true);

    snprintf(buf, sizeof(buf), "%.2fV %+.3fA %.1fW",
             state->latest_telemetry.ina2_bus_voltage_v,
             state->latest_telemetry.ina2_current_a,
             state->latest_telemetry.ina2_power_w);
    fb_draw_string(2, 46, buf, 1, true);

    snprintf(buf, sizeof(buf), "Sh:%.1fmV / %.1fmV",
             state->latest_telemetry.ina1_shunt_mv,
             state->latest_telemetry.ina2_shunt_mv);
    fb_draw_string(2, 56, buf, 1, true);
}

static void render_lem(const system_state_t *state) {
    char buf[64];

    snprintf(buf, sizeof(buf), "DHAB S/157 %s", state->dhab_invert ? "[INV]" : "");
    fb_draw_string(2, 12, buf, 1, true);

    snprintf(buf, sizeof(buf), "Lo:%+.2fA Hi:%+.1fA",
             state->latest_telemetry.dhab_ch1_low_a,
             state->latest_telemetry.dhab_ch2_high_a);
    fb_draw_string(2, 22, buf, 1, true);

    fb_draw_hline(0, 32, LCD_W, true);

    snprintf(buf, sizeof(buf), "HO-100-S %s", state->ho100_invert ? "[INV]" : "");
    fb_draw_string(2, 35, buf, 1, true);

    snprintf(buf, sizeof(buf), "I:%+.2fA  V:%.2fV",
             state->latest_telemetry.ho100_current_a,
             state->latest_telemetry.ho100_raw_v);
    fb_draw_string(2, 45, buf, 1, true);

    fb_draw_string_centered(55, "Hold A3: Flip +/-", 1, true);
}

static void render_wifi(const system_state_t *state) {
    fb_draw_string_centered(13, state->wifi_portal_active ? "WIFI: ACTIVE [ON]" : "WIFI: OFF (SAVING)", 1, true);
    fb_draw_string_centered(25, "SSID: ClampMeter-WiFi", 1, true);
    fb_draw_string_centered(36, state->wifi_portal_active ? "URL: 192.168.4.1" : "Access CSV via Web", 1, true);
    fb_draw_string_centered(52, state->wifi_portal_active ? "[A2] Turn WiFi OFF" : "[A2] Turn WiFi ON", 1, true);
}

static void render_system(const system_state_t *state) {
    char buf[64];

    if (state->battery_present && state->latest_telemetry.battery_v >= 2.50f) {
        snprintf(buf, sizeof(buf), "Batt: %.2fV (%u%%)",
                 state->latest_telemetry.battery_v, state->latest_telemetry.battery_pct);
    } else {
        snprintf(buf, sizeof(buf), "Batt: NO BATT (USB)");
    }
    fb_draw_string(2, 12, buf, 1, true);

    snprintf(buf, sizeof(buf), "SD: %s", state->sd_mounted ? "MOUNTED" : "NO CARD");
    fb_draw_string(2, 22, buf, 1, true);

    snprintf(buf, sizeof(buf), "Recs:%lu Drp:%lu",
             (unsigned long)state->records_written,
             (unsigned long)state->queue_overflow_count);
    fb_draw_string(2, 32, buf, 1, true);

    snprintf(buf, sizeof(buf), "Mode: %s",
             state->active_mode == LOG_MODE_TIMED ? "TIMED" : "UNTIL DEAD");
    fb_draw_string(2, 42, buf, 1, true);

    if (state->is_logging) {
        snprintf(buf, sizeof(buf), "Time: %lus [A2:Stop]", (unsigned long)state->elapsed_sec);
        fb_draw_string(2, 53, buf, 1, true);
    } else {
        fb_draw_string(2, 53, "[A2:Log] [A1:Zero]", 1, true);
    }
}

void display_st7920_update(const system_state_t *state) {
    if (!s_spi_dev || !state) return;

    fb_clear();
    render_top_bar(state);

    switch (state->ui_screen) {
        case 1:
            render_ina(state);
            break;
        case 2:
            render_lem(state);
            break;
        case 3:
            render_wifi(state);
            break;
        case 4:
            render_system(state);
            break;
        case 0:
        default:
            render_overview(state);
            break;
    }

    if (state->zero_popup_timer > 0) {
        fb_fill_rect(8, 16, 112, 32, false);
        fb_draw_rect(8, 16, 112, 32, true);
        fb_draw_string_centered(22, state->is_zeroed ? "ZERO / TARE SET!" : "ZERO CLEARED!", 1, true);
        fb_draw_string_centered(34, state->is_zeroed ? "Offsets calibrated" : "Raw values active", 1, true);
    } else if (state->invert_popup_timer > 0) {
        char inv_buf[32];
        fb_fill_rect(8, 16, 112, 32, false);
        fb_draw_rect(8, 16, 112, 32, true);
        fb_draw_string_centered(22, "LEM POLARITY", 1, true);
        snprintf(inv_buf, sizeof(inv_buf), "DHAB:%s HO:%s",
                 state->dhab_invert ? "INV" : "NORM",
                 state->ho100_invert ? "INV" : "NORM");
        fb_draw_string_centered(34, inv_buf, 1, true);
    }

    st7920_flush_dirty();
}
