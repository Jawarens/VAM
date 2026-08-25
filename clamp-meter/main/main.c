/*
 * ESP32-C3-LCDKit LEM HO-100-S Clamp Meter & Data Logger
 * M5Stack-Inspired Telemetry UI with Transparent Card Layout,
 * True GC9A01 BGR Palette (Pure Red 0x001F Negative, Electric Green 0x07E0 Positive),
 * Extra-Wide 168px Current Box for 7-Digit Readouts (-150.50A),
 * High-Visibility White Status Prompts & Zero Dial Arc Overlap
 * 
 * Hardware Mapping:
 * - Analog In (ADC1_CH4): GPIO4 (LEM HO-100-S via 10k/20k voltage divider)
 * - Buzzer:               GPIO3 (LEDC PWM with LOW / MED / HIGH volume)
 * - Onboard RGB LED:      GPIO8 (WS2812 Low Power State Indicator)
 * - Temp Sensor:          ESP32-C3 On-Chip Hardware Sensor
 * - GC9A01 240x240 LCD:
 *     - MOSI: GPIO0, SCLK: GPIO1, DC: GPIO2, CS: GPIO7, BL: GPIO5
 * - Rotary Encoder:
 *     - Enc A: GPIO6, Enc B: GPIO10, Enc SW: GPIO9
 * - Storage:
 *     - SPIFFS at "/spiffs" (Configurable CSV recording)
 * - Wi-Fi Web Server:
 *     - SoftAP SSID: "ClampMeter-WiFi", Web Portal: http://192.168.4.1
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/temperature_sensor.h"
#include "driver/spi_master.h"
#include "led_strip.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"

static const char *TAG = "CLAMP_METER";

#define PIN_ADC_CLAMP       ADC_CHANNEL_4 // GPIO4 (ADC1 Channel 4)
#define PIN_BUZZER          GPIO_NUM_3    // GPIO3 (Buzzer)
#define PIN_RGB_LED         GPIO_NUM_8    // GPIO8 (WS2812 RGB LED)
#define PIN_ENC_A           GPIO_NUM_6
#define PIN_ENC_B           GPIO_NUM_10
#define PIN_ENC_SW          GPIO_NUM_9

#define PIN_LCD_MOSI        GPIO_NUM_0
#define PIN_LCD_SCLK        GPIO_NUM_1
#define PIN_LCD_DC          GPIO_NUM_2
#define PIN_LCD_CS          GPIO_NUM_7
#define PIN_LCD_BL          GPIO_NUM_5

#define LCD_H_RES           240
#define LCD_V_RES           240

// HO-100-S Transfer Function constants (5V sensor, with 10k/20k divider: factor = 20/30 = 0.66667)
// Vout = 2.5V + (I * 0.0125V/A)
// Vadc = Vout * (20k / (10k + 20k)) = Vout * 0.66667
#define DIVIDER_RATIO       (20.0f / 30.0f)
#define VREF_ZERO_DEFAULT   (2.500f)
#define SENSITIVITY_V_PER_A (0.0125f)

// GC9A01 BGR Native Color Palette
#define C_BG            0x0000  // Deep OLED Black
#define C_CARD_BORDER   0x4A49  // Slate Outline
#define C_WHITE         0xFFFF  // Pure Crisp White
#define C_BLACK         0x0000  // Pure Black
#define C_CYAN          0xFFE0  // Electric Cyan (Blue + Green)
#define C_GREEN         0x07E0  // Pure Electric Green (Positive Current)
#define C_ORANGE        0x05DF  // Warm Amber
#define C_RED           0x001F  // Pure Crimson Red (Negative Current)
#define C_YELLOW        0x07FF  // Bright Yellow (Red + Green)
#define C_LIGHT_GRAY    0xC618  // High-Visibility Slate Gray
#define C_PURPLE        0xF81F  // Purple (Red + Blue)
#define C_MENU_BG       0x0000  // Black Modal Background
#define C_MENU_SEL      0x05DF  // Amber Selected Highlight

typedef enum {
    BEEP_VOL_MUTE = 0,
    BEEP_VOL_LOW,
    BEEP_VOL_MED,
    BEEP_VOL_HIGH,
    BEEP_VOL_MAX
} BeepVolume_t;

typedef enum {
    MENU_ITEM_ZERO = 0,
    MENU_ITEM_RESET_PEAK,
    MENU_ITEM_INTERVAL,
    MENU_ITEM_VOLUME,
    MENU_ITEM_WIFI_SERVER,
    MENU_ITEM_ERASE_LOGS,
    MENU_ITEM_EXIT,
    MENU_ITEM_MAX
} MenuItem_t;

typedef struct {
    float current_A;
    float peak_A;
    float min_A;
    float sensor_voltage_V;
    float zero_offset_V;
    float temp_C;
    bool is_recording;
    BeepVolume_t beep_volume;
    uint32_t rec_start_time_s;
    uint32_t rec_elapsed_s;
    uint32_t log_count;
    uint32_t log_interval_s;
    char current_log_filename[32];
    FILE *log_file;
    char notification_msg[32];
    uint32_t notification_until_ms;

    // Menu & Wi-Fi State
    bool in_menu;
    bool in_wifi_server;
    int menu_cursor;
    uint32_t menu_last_activity_ms;
} MeterState_t;

static MeterState_t g_meter = {
    .current_A = 0.0f,
    .peak_A = 0.0f,
    .min_A = 0.0f,
    .sensor_voltage_V = 2.500f,
    .zero_offset_V = 2.500f,
    .temp_C = 25.0f,
    .is_recording = false,
    .beep_volume = BEEP_VOL_LOW,
    .rec_start_time_s = 0,
    .rec_elapsed_s = 0,
    .log_count = 0,
    .log_interval_s = 10,
    .log_file = NULL,
    .notification_msg = "",
    .notification_until_ms = 0,
    .in_menu = false,
    .in_wifi_server = false,
    .menu_cursor = 0,
    .menu_last_activity_ms = 0
};

static SemaphoreHandle_t g_mutex;
static adc_oneshot_unit_handle_t g_adc1_handle;
static adc_cali_handle_t g_adc1_cali_handle = NULL;
static esp_lcd_panel_handle_t g_panel_handle = NULL;
static temperature_sensor_handle_t g_temp_sensor = NULL;
static led_strip_handle_t g_led_strip = NULL;
static httpd_handle_t g_http_server = NULL;
static esp_netif_t *g_ap_netif = NULL;

static uint16_t *s_fb = NULL;

/* -------------------------------------------------------------------------- */
/*                              Onboard RGB LED                               */
/* -------------------------------------------------------------------------- */
static void init_rgb_led(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = PIN_RGB_LED,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
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
    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &g_led_strip);
    if (ret == ESP_OK && g_led_strip) {
        led_strip_clear(g_led_strip);
        ESP_LOGI(TAG, "Onboard Low-Power RGB LED initialized.");
    } else {
        ESP_LOGW(TAG, "RGB LED init skipped (%s).", esp_err_to_name(ret));
    }
}

static void update_rgb_led_state(void)
{
    if (!g_led_strip) return;

    if (g_meter.in_wifi_server) {
        led_strip_set_pixel(g_led_strip, 0, 8, 0, 14);
    } else if (g_meter.is_recording) {
        uint32_t ms = (esp_timer_get_time() / 1000) % 1000;
        uint8_t brightness = (ms < 500) ? (ms * 16 / 500) : ((1000 - ms) * 16 / 500);
        if (brightness < 3) brightness = 3;
        led_strip_set_pixel(g_led_strip, 0, brightness, 0, 0);
    } else if (g_meter.in_menu) {
        led_strip_set_pixel(g_led_strip, 0, 10, 6, 0);
    } else {
        led_strip_set_pixel(g_led_strip, 0, 0, 3, 10);
    }
    led_strip_refresh(g_led_strip);
}

/* -------------------------------------------------------------------------- */
/*                                Buzzer Driver                               */
/* -------------------------------------------------------------------------- */
static void init_buzzer(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LEDC_TIMER_0,
        .duty_resolution  = LEDC_TIMER_10_BIT,
        .freq_hz          = 2700,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = PIN_BUZZER,
        .duty           = 0,
        .hpoint         = 0
    };
    ledc_channel_config(&ledc_channel);
}

static uint32_t get_volume_duty(void)
{
    switch (g_meter.beep_volume) {
    case BEEP_VOL_LOW:  return 64;
    case BEEP_VOL_MED:  return 220;
    case BEEP_VOL_HIGH: return 512;
    case BEEP_VOL_MUTE:
    default:            return 0;
    }
}

static void buzzer_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    uint32_t duty = get_volume_duty();
    if (duty == 0 || freq_hz == 0) return;

    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, freq_hz);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void buzzer_click(void)
{
    buzzer_tone(2800, 8);
}

static void buzzer_confirm(void)
{
    buzzer_tone(2400, 40);
    vTaskDelay(pdMS_TO_TICKS(20));
    buzzer_tone(3600, 60);
}

static void buzzer_cancel(void)
{
    buzzer_tone(3200, 40);
    vTaskDelay(pdMS_TO_TICKS(20));
    buzzer_tone(1800, 60);
}

/* -------------------------------------------------------------------------- */
/*                            Temperature Sensor                              */
/* -------------------------------------------------------------------------- */
static void init_temperature(void)
{
    temperature_sensor_config_t temp_sensor_config = {
        .range_min = -10,
        .range_max = 80,
    };
    if (temperature_sensor_install(&temp_sensor_config, &g_temp_sensor) == ESP_OK) {
        temperature_sensor_enable(g_temp_sensor);
        ESP_LOGI(TAG, "Hardware Temperature Sensor initialized.");
    }
}

/* -------------------------------------------------------------------------- */
/*                                SPIFFS Storage                              */
/* -------------------------------------------------------------------------- */
static void init_spiffs(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS...");
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 10,
        .format_if_mount_failed = true
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS (%s)", esp_err_to_name(ret));
    } else {
        size_t total = 0, used = 0;
        esp_spiffs_info("storage", &total, &used);
        ESP_LOGI(TAG, "SPIFFS mounted: total %d KB, used %d KB", (int)(total / 1024), (int)(used / 1024));
    }
}

static void start_new_log_file(void)
{
    int file_idx = 1;
    struct stat st;
    while (file_idx < 1000) {
        snprintf(g_meter.current_log_filename, sizeof(g_meter.current_log_filename),
                 "/spiffs/clamp_%03d.csv", file_idx);
        if (stat(g_meter.current_log_filename, &st) != 0) {
            break;
        }
        file_idx++;
    }

    g_meter.log_file = fopen(g_meter.current_log_filename, "w");
    if (g_meter.log_file) {
        fprintf(g_meter.log_file, "timestamp_s,current_A,sensor_V,temp_C\n");
        fflush(g_meter.log_file);
        ESP_LOGI(TAG, "Started new log: %s", g_meter.current_log_filename);
    } else {
        ESP_LOGE(TAG, "Failed to create log file: %s", g_meter.current_log_filename);
    }
}

static void stop_log_file(void)
{
    if (g_meter.log_file) {
        fclose(g_meter.log_file);
        g_meter.log_file = NULL;
        ESP_LOGI(TAG, "Closed log: %s", g_meter.current_log_filename);
    }
}

static void erase_all_logs(void)
{
    DIR *d = opendir("/spiffs");
    if (!d) return;
    struct dirent *dir;
    char filepath[300];
    while ((dir = readdir(d)) != NULL) {
        if (strstr(dir->d_name, ".csv") != NULL) {
            snprintf(filepath, sizeof(filepath), "/spiffs/%s", dir->d_name);
            unlink(filepath);
            ESP_LOGI(TAG, "Deleted: %s", filepath);
        }
    }
    closedir(d);
}

/* -------------------------------------------------------------------------- */
/*                            Wi-Fi & Web Log Server                          */
/* -------------------------------------------------------------------------- */
static const char *HTML_HEADER = 
"<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>LEM HO-100-S Clamp Meter Logs</title>"
"<style>"
"body{background:#0b0f19;color:#e2e8f0;font-family:system-ui,-apple-system,sans-serif;margin:0;padding:20px;text-align:center}"
".card{background:#1a2234;border:1px solid #2d3748;border-radius:12px;max-width:600px;margin:20px auto;padding:24px;text-align:left;box-shadow:0 8px 24px rgba(0,0,0,0.5)}"
"h1{color:#38bdf8;font-size:22px;margin-top:0;display:flex;align-items:center;gap:8px}"
"table{width:100%;border-collapse:collapse;margin:16px 0}"
"th,td{padding:10px 12px;border-bottom:1px solid #2d3748;font-size:14px}"
"th{background:#232d42;color:#94a3b8;text-transform:uppercase;font-size:12px}"
".btn{background:#0284c7;color:#fff;text-decoration:none;padding:6px 12px;border-radius:6px;font-size:13px;display:inline-block;margin-right:6px}"
".btn-del{background:#dc2626}"
"</style></head><body>"
"<div class='card'>"
"<h1>⚡ LEM HO-100-S Log Manager</h1>"
"<p style='color:#94a3b8;font-size:14px'>Stored Telemetry CSV Files on SPIFFS</p>"
"<table><tr><th>Filename</th><th>Size</th><th>Action</th></tr>";

static const char *HTML_FOOTER = 
"</table>"
"<div style='margin-top:20px;text-align:center'>"
"<a href='/' class='btn' style='background:#475569'>🔄 Refresh List</a>"
"</div></div></body></html>";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, HTML_HEADER, strlen(HTML_HEADER));

    DIR *d = opendir("/spiffs");
    if (d) {
        struct dirent *dir;
        char row[1024];
        struct stat st;
        char filepath[300];
        int count = 0;

        while ((dir = readdir(d)) != NULL) {
            if (strstr(dir->d_name, ".csv") != NULL) {
                snprintf(filepath, sizeof(filepath), "/spiffs/%s", dir->d_name);
                size_t sz = 0;
                if (stat(filepath, &st) == 0) {
                    sz = st.st_size;
                }
                snprintf(row, sizeof(row),
                         "<tr><td><b>%.64s</b></td><td>%.1f KB</td><td>"
                         "<a class='btn' href='/download?file=%.64s'>⬇ Download</a>"
                         "<a class='btn btn-del' href='/delete?file=%.64s' onclick=\"return confirm('Delete this log?')\">🗑</a>"
                         "</td></tr>",
                         dir->d_name, (float)sz / 1024.0f, dir->d_name, dir->d_name);
                httpd_resp_send_chunk(req, row, strlen(row));
                count++;
            }
        }
        closedir(d);
        if (count == 0) {
            const char *empty_row = "<tr><td colspan='3' style='text-align:center;color:#64748b'>No CSV logs found on device.</td></tr>";
            httpd_resp_send_chunk(req, empty_row, strlen(empty_row));
        }
    }

    httpd_resp_send_chunk(req, HTML_FOOTER, strlen(HTML_FOOTER));
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t download_get_handler(httpd_req_t *req)
{
    char buf[128];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char filename[64];
        if (httpd_query_key_value(buf, "file", filename, sizeof(filename)) == ESP_OK) {
            char filepath[300];
            snprintf(filepath, sizeof(filepath), "/spiffs/%s", filename);
            FILE *f = fopen(filepath, "r");
            if (f) {
                httpd_resp_set_type(req, "text/csv");
                char disp[128];
                snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", filename);
                httpd_resp_set_hdr(req, "Content-Disposition", disp);

                char chunk[512];
                size_t read_bytes;
                while ((read_bytes = fread(chunk, 1, sizeof(chunk), f)) > 0) {
                    httpd_resp_send_chunk(req, chunk, read_bytes);
                }
                fclose(f);
                httpd_resp_send_chunk(req, NULL, 0);
                return ESP_OK;
            }
        }
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

static esp_err_t delete_get_handler(httpd_req_t *req)
{
    char buf[128];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char filename[64];
        if (httpd_query_key_value(buf, "file", filename, sizeof(filename)) == ESP_OK) {
            char filepath[300];
            snprintf(filepath, sizeof(filepath), "/spiffs/%s", filename);
            unlink(filepath);
            ESP_LOGI(TAG, "Web deleted: %s", filepath);
        }
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void start_wifi_web_server(void)
{
    if (g_meter.in_wifi_server) return;

    ESP_LOGI(TAG, "Starting Wi-Fi Access Point & Web Server...");
    esp_netif_init();
    esp_event_loop_create_default();
    g_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "ClampMeter-WiFi",
            .ssid_len = strlen("ClampMeter-WiFi"),
            .channel = 1,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN
        },
    };

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    if (httpd_start(&g_http_server, &config) == ESP_OK) {
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_http_server, &root_uri);

        httpd_uri_t download_uri = {
            .uri = "/download",
            .method = HTTP_GET,
            .handler = download_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_http_server, &download_uri);

        httpd_uri_t del_uri = {
            .uri = "/delete",
            .method = HTTP_GET,
            .handler = delete_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(g_http_server, &del_uri);
        ESP_LOGI(TAG, "HTTP Server active on http://192.168.4.1");
    }
    g_meter.in_wifi_server = true;
}

static void stop_wifi_web_server(void)
{
    if (!g_meter.in_wifi_server) return;

    if (g_http_server) {
        httpd_stop(g_http_server);
        g_http_server = NULL;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    g_meter.in_wifi_server = false;
    ESP_LOGI(TAG, "Wi-Fi Server stopped.");
}

/* -------------------------------------------------------------------------- */
/*                                ADC Sampling                                */
/* -------------------------------------------------------------------------- */
static void init_adc(void)
{
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &g_adc1_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(g_adc1_handle, PIN_ADC_CLAMP, &config));

    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .chan = PIN_ADC_CLAMP,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &g_adc1_cali_handle) == ESP_OK) {
        ESP_LOGI(TAG, "ADC Calibration curve fitting initialized");
    }
}

static void read_clamp_telemetry(float *out_amps, float *out_sensor_v, float *out_temp_c)
{
    int raw = 0;
    int voltage_mv = 0;
    const int OVERSAMPLE_COUNT = 64;
    int accum_mv = 0;

    for (int i = 0; i < OVERSAMPLE_COUNT; i++) {
        adc_oneshot_read(g_adc1_handle, PIN_ADC_CLAMP, &raw);
        if (g_adc1_cali_handle) {
            adc_cali_raw_to_voltage(g_adc1_cali_handle, raw, &voltage_mv);
            accum_mv += voltage_mv;
        } else {
            accum_mv += (raw * 3300) / 4095;
        }
    }
    float avg_vadc = (accum_mv / (float)OVERSAMPLE_COUNT) / 1000.0f;
    float v_sensor_out = avg_vadc / DIVIDER_RATIO;
    float current_amps = (v_sensor_out - g_meter.zero_offset_V) / SENSITIVITY_V_PER_A;

    *out_amps = current_amps;
    *out_sensor_v = v_sensor_out;

    if (g_temp_sensor) {
        temperature_sensor_get_celsius(g_temp_sensor, out_temp_c);
    } else {
        *out_temp_c = 25.0f;
    }
}

/* -------------------------------------------------------------------------- */
/*                         Graphics Primitives & Canvas                       */
/* -------------------------------------------------------------------------- */

static const uint8_t font5x7[] = {
    0x00,0x00,0x00,0x00,0x00, //   0x20
    0x00,0x00,0x5F,0x00,0x00, // ! 0x21
    0x00,0x07,0x00,0x07,0x00, // " 0x22
    0x14,0x7F,0x14,0x7F,0x14, // # 0x23
    0x24,0x2A,0x7F,0x2A,0x12, // $ 0x24
    0x23,0x13,0x08,0x64,0x62, // % 0x25
    0x36,0x49,0x55,0x22,0x50, // & 0x26
    0x00,0x05,0x03,0x00,0x00, // ' 0x27
    0x00,0x1C,0x22,0x41,0x00, // ( 0x28
    0x00,0x41,0x22,0x1C,0x00, // ) 0x29
    0x14,0x08,0x3E,0x08,0x14, // * 0x2A
    0x08,0x08,0x3E,0x08,0x08, // + 0x2B
    0x00,0x50,0x30,0x00,0x00, // , 0x2C
    0x08,0x08,0x08,0x08,0x08, // - 0x2D
    0x00,0x60,0x60,0x00,0x00, // . 0x2E
    0x20,0x10,0x08,0x04,0x02, // / 0x2F
    0x3E,0x51,0x49,0x45,0x3E, // 0 0x30
    0x00,0x42,0x7F,0x40,0x00, // 1 0x31
    0x42,0x61,0x51,0x49,0x46, // 2 0x32
    0x21,0x41,0x45,0x4B,0x31, // 3 0x33
    0x18,0x14,0x12,0x7F,0x10, // 4 0x34
    0x27,0x45,0x45,0x45,0x39, // 5 0x35
    0x3C,0x4A,0x49,0x49,0x30, // 6 0x36
    0x01,0x71,0x09,0x05,0x03, // 7 0x37
    0x36,0x49,0x49,0x49,0x36, // 8 0x38
    0x06,0x49,0x49,0x29,0x1E, // 9 0x39
    0x00,0x36,0x36,0x00,0x00, // : 0x3A
    0x00,0x56,0x36,0x00,0x00, // ; 0x3B
    0x08,0x14,0x22,0x41,0x00, // < 0x3C
    0x14,0x14,0x14,0x14,0x14, // = 0x3D
    0x00,0x41,0x22,0x14,0x08, // > 0x3E
    0x02,0x01,0x51,0x09,0x06, // ? 0x3F
    0x32,0x49,0x79,0x41,0x3E, // @ 0x40
    0x7E,0x11,0x11,0x11,0x7E, // A 0x41
    0x7F,0x49,0x49,0x49,0x36, // B 0x42
    0x3E,0x41,0x41,0x41,0x22, // C 0x43
    0x7F,0x41,0x41,0x22,0x1C, // D 0x44
    0x7F,0x49,0x49,0x49,0x41, // E 0x45
    0x7F,0x09,0x09,0x09,0x01, // F 0x46
    0x3E,0x41,0x49,0x49,0x7A, // G 0x47
    0x7F,0x08,0x08,0x08,0x7F, // H 0x48
    0x00,0x41,0x7F,0x41,0x00, // I 0x49
    0x20,0x40,0x41,0x3F,0x01, // J 0x4A
    0x7F,0x08,0x14,0x22,0x41, // K 0x4B
    0x7F,0x40,0x40,0x40,0x40, // L 0x4C
    0x7F,0x02,0x0C,0x02,0x7F, // M 0x4D
    0x7F,0x04,0x08,0x10,0x7F, // N 0x4E
    0x3E,0x41,0x41,0x41,0x3E, // O 0x4F
    0x7F,0x09,0x09,0x09,0x06, // P 0x50
    0x3E,0x41,0x51,0x21,0x5E, // Q 0x51
    0x7F,0x09,0x19,0x29,0x46, // R 0x52
    0x46,0x49,0x49,0x49,0x31, // S 0x53
    0x01,0x01,0x7F,0x01,0x01, // T 0x54
    0x3F,0x40,0x40,0x40,0x3F, // U 0x55
    0x1F,0x20,0x40,0x20,0x1F, // V 0x56
    0x3F,0x40,0x38,0x40,0x3F, // W 0x57
    0x63,0x14,0x08,0x14,0x63, // X 0x58
    0x07,0x08,0x70,0x08,0x07, // Y 0x59
    0x61,0x51,0x49,0x45,0x43  // Z 0x5A
};

static inline void fb_set_pixel(int x, int y, uint16_t color)
{
    if (x >= 0 && x < LCD_H_RES && y >= 0 && y < LCD_V_RES) {
        s_fb[y * LCD_H_RES + x] = color;
    }
}

static void fb_clear(uint16_t color)
{
    if (!s_fb) return;
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) {
        s_fb[i] = color;
    }
}

static void fb_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    for (int j = y; j < y + h; j++) {
        if (j >= 0 && j < LCD_V_RES) {
            for (int i = x; i < x + w; i++) {
                if (i >= 0 && i < LCD_H_RES) {
                    s_fb[j * LCD_H_RES + i] = color;
                }
            }
        }
    }
}

static void fb_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    for (int i = x; i < x + w; i++) {
        fb_set_pixel(i, y, color);
        fb_set_pixel(i, y + h - 1, color);
    }
    for (int j = y; j < y + h; j++) {
        fb_set_pixel(x, j, color);
        fb_set_pixel(x + w - 1, j, color);
    }
}

static void fb_draw_char(int x, int y, char c, uint16_t color, int scale)
{
    if (c < 32 || c > 90) {
        if (c >= 'a' && c <= 'z') c -= 32;
        else c = '?';
    }
    const uint8_t *bitmap = &font5x7[(c - 32) * 5];
    for (int i = 0; i < 5; i++) {
        uint8_t col = bitmap[i];
        for (int j = 0; j < 7; j++) {
            if (col & (1 << j)) {
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

static void fb_draw_string(int x, int y, const char *str, uint16_t color, int scale)
{
    int cur_x = x;
    while (*str) {
        fb_draw_char(cur_x, y, *str, color, scale);
        cur_x += (5 * scale) + (1 * scale);
        str++;
    }
}

static void fb_draw_string_centered(int cx, int y, const char *str, uint16_t color, int scale)
{
    int len = strlen(str);
    int total_width = len * 6 * scale;
    int start_x = cx - (total_width / 2);
    fb_draw_string(start_x, y, str, color, scale);
}

static void fb_draw_gauge_arc(int cx, int cy, int r_outer, int r_inner, float angle_start, float angle_end, uint16_t color)
{
    float step = 0.005f;
    for (float a = angle_start; a <= angle_end; a += step) {
        float cos_a = cosf(a);
        float sin_a = sinf(a);
        for (int r = r_inner; r <= r_outer; r++) {
            int px = cx + (int)(r * cos_a);
            int py = cy + (int)(r * sin_a);
            fb_set_pixel(px, py, color);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*                                LCD Device                                  */
/* -------------------------------------------------------------------------- */
static void init_lcd(void)
{
    // 1. Allocate 240x240 Frame Buffer first before any peripheral consumes RAM
    s_fb = (uint16_t *)heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_fb) {
        s_fb = (uint16_t *)malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t));
    }
    if (s_fb) {
        ESP_LOGI(TAG, "Frame buffer allocated (115.2 KB) @ %p", s_fb);
        memset(s_fb, 0, LCD_H_RES * LCD_V_RES * sizeof(uint16_t));
    } else {
        ESP_LOGE(TAG, "FATAL: Could not allocate frame buffer!");
    }

    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_LCD_BL
    };
    gpio_config(&bk_gpio_config);
    gpio_set_level(PIN_LCD_BL, 1);

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &g_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(g_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(g_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(g_panel_handle, true)); // True Black
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(g_panel_handle, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(g_panel_handle, true));
}

/* -------------------------------------------------------------------------- */
/*                            Hold Menu Rendering                             */
/* -------------------------------------------------------------------------- */
static const char *menu_titles[MENU_ITEM_MAX] = {
    "1. ZERO CLAMP",
    "2. RESET PEAK",
    "3. LOG INTERVAL",
    "4. BEEP VOLUME",
    "5. WIFI LOG SERVER",
    "6. ERASE LOGS",
    "7. EXIT MENU"
};

static const char *volume_names[BEEP_VOL_MAX] = {
    "MUTE", "LOW", "MED", "HIGH"
};

static void draw_hold_menu(void)
{
    int cx = 120;

    // Pure black modal frame
    fb_fill_rect(20, 20, 200, 200, C_BLACK);
    fb_draw_rect(20, 20, 200, 200, C_CYAN);
    fb_draw_rect(22, 22, 196, 196, C_CARD_BORDER);

    fb_draw_string_centered(cx, 28, "- QUICK MENU -", C_YELLOW, 1);

    int start_y = 48;
    int item_height = 22;

    for (int i = 0; i < MENU_ITEM_MAX; i++) {
        int item_y = start_y + i * item_height;
        bool is_selected = (i == g_meter.menu_cursor);

        if (is_selected) {
            fb_fill_rect(26, item_y - 2, 188, 18, C_MENU_SEL);
            fb_draw_rect(26, item_y - 2, 188, 18, C_WHITE);
        }

        uint16_t text_col = is_selected ? C_BLACK : C_WHITE;

        if (i == MENU_ITEM_INTERVAL) {
            char interval_str[32];
            snprintf(interval_str, sizeof(interval_str), "3. INTERVAL: %lus", g_meter.log_interval_s);
            fb_draw_string(30, item_y + 2, interval_str, text_col, 1);
        } else if (i == MENU_ITEM_VOLUME) {
            char vol_str[32];
            snprintf(vol_str, sizeof(vol_str), "4. BEEP: %s", volume_names[g_meter.beep_volume]);
            fb_draw_string(30, item_y + 2, vol_str, text_col, 1);
        } else {
            fb_draw_string(30, item_y + 2, menu_titles[i], text_col, 1);
        }
    }

    fb_draw_string_centered(cx, 204, "TURN:NAV | CLICK:OK", C_CYAN, 1);
}

/* -------------------------------------------------------------------------- */
/*                            Wi-Fi Server Screen                             */
/* -------------------------------------------------------------------------- */
static void draw_wifi_server_screen(void)
{
    int cx = 120;

    fb_draw_gauge_arc(cx, cx, 116, 110, 0, 6.283f, C_PURPLE);

    fb_draw_rect(28, 28, 184, 34, C_PURPLE);
    fb_draw_string_centered(cx, 34, "WIFI LOG SERVER", C_PURPLE, 1);
    fb_draw_string_centered(cx, 48, "PORTAL ACTIVE", C_GREEN, 1);

    fb_draw_rect(24, 72, 192, 94, C_CARD_BORDER);

    fb_draw_string(32, 80, "1. CONNECT TO WIFI:", C_WHITE, 1);
    fb_draw_string(32, 94, "ClampMeter-WiFi", C_YELLOW, 1);

    fb_draw_string(32, 116, "2. OPEN BROWSER URL:", C_WHITE, 1);
    fb_draw_string(32, 130, "http://192.168.4.1", C_CYAN, 1);

    fb_draw_string(32, 148, "DOWNLOAD CSV FILES", C_WHITE, 1);

    fb_draw_string_centered(cx, 188, "CLICK KNOB TO EXIT", C_ORANGE, 1);
}

/* -------------------------------------------------------------------------- */
/*                            Telemetry Screen                                */
/* -------------------------------------------------------------------------- */
static void draw_telemetry_screen(void)
{
    int cx = 120;
    int cy = 120;

    // Outer Circular Dial Track (Clear & Unobstructed at r=116..110)
    float angle_min = 2.356f;
    float angle_max = 7.068f;
    fb_draw_gauge_arc(cx, cy, 116, 110, angle_min, angle_max, C_CARD_BORDER);

    float current_abs = fabsf(g_meter.current_A);
    float pct = current_abs / 100.0f;
    if (pct > 1.0f) pct = 1.0f;

    uint16_t gauge_color = C_CYAN;
    if (pct > 0.8f) gauge_color = C_RED;
    else if (pct > 0.5f) gauge_color = C_ORANGE;

    float angle_active = angle_min + pct * (angle_max - angle_min);
    if (pct > 0.01f) {
        fb_draw_gauge_arc(cx, cy, 116, 110, angle_min, angle_active, gauge_color);
    }

    // Top Header Banner (y=16, y=28)
    fb_draw_string_centered(cx, 16, "LEM HO-100-S", C_WHITE, 1);

    char rec_str[24];
    uint32_t now_ms = esp_timer_get_time() / 1000;
    bool blink = (now_ms / 500) % 2;

    if (g_meter.is_recording) {
        uint32_t m = g_meter.rec_elapsed_s / 60;
        uint32_t s = g_meter.rec_elapsed_s % 60;
        snprintf(rec_str, sizeof(rec_str), "REC %02lu:%02lu", m, s);
        uint16_t tag_color = blink ? C_RED : C_YELLOW;
        fb_draw_string_centered(cx, 30, rec_str, tag_color, 1);
    } else {
        fb_draw_string_centered(cx, 30, "STANDBY", C_WHITE, 1);
    }

    // Center Telemetry Card: Width 168px (x=36..204, y=50..112) - comfortably fits 8 chars (-150.50A)!
    fb_draw_rect(36, 50, 168, 62, C_CYAN);
    fb_draw_string_centered(cx, 54, "DC CURRENT", C_LIGHT_GRAY, 1);

    char val_str[16];
    snprintf(val_str, sizeof(val_str), "%+.2f", g_meter.current_A);
    
    // Polarity: Pure Crimson Red (0x001F) when Negative, Electric Green (0x07E0) when Positive
    uint16_t num_color = (g_meter.current_A >= 0.0f) ? C_GREEN : C_RED;

    fb_draw_string_centered(cx - 16, 72, val_str, num_color, 3);
    fb_draw_string(cx + 60, 80, "A", C_WHITE, 2);

    // Lower Telemetry 3-Grid (y=122..158, inside x=44..196)
    fb_draw_rect(44, 122, 46, 36, C_CARD_BORDER);
    fb_draw_string(48, 126, "PEAK", C_LIGHT_GRAY, 1);
    char peak_str[12];
    snprintf(peak_str, sizeof(peak_str), "%.1fA", g_meter.peak_A);
    fb_draw_string(48, 140, peak_str, C_YELLOW, 1);

    fb_draw_rect(97, 122, 46, 36, C_CARD_BORDER);
    fb_draw_string(101, 126, "VSEN", C_LIGHT_GRAY, 1);
    char v_str[12];
    snprintf(v_str, sizeof(v_str), "%.2fV", g_meter.sensor_voltage_V);
    fb_draw_string(101, 140, v_str, C_CYAN, 1);

    fb_draw_rect(150, 122, 46, 36, C_CARD_BORDER);
    fb_draw_string(154, 126, "TEMP", C_LIGHT_GRAY, 1);
    char temp_str[12];
    snprintf(temp_str, sizeof(temp_str), "%.0fC", g_meter.temp_C);
    fb_draw_string(154, 140, temp_str, C_WHITE, 1);

    // Bottom Status Bar: In-between the lower circular arc opening (y=186, crisp white text)
    char log_info[32];
    if (g_meter.is_recording) {
        snprintf(log_info, sizeof(log_info), "LOGS: %lu (%lus)", g_meter.log_count, g_meter.log_interval_s);
        fb_draw_string_centered(cx, 186, log_info, C_GREEN, 1);
    } else {
        fb_draw_string_centered(cx, 186, "CLICK:LOG | HOLD:MENU", C_WHITE, 1);
    }

    // Notification Banner Toast (y=196)
    if (now_ms < g_meter.notification_until_ms && g_meter.notification_msg[0] != '\0') {
        fb_fill_rect(24, 196, 192, 26, C_YELLOW);
        fb_draw_rect(24, 196, 192, 26, C_WHITE);
        fb_draw_string_centered(cx, 203, g_meter.notification_msg, C_BLACK, 1);
    }
}

/* -------------------------------------------------------------------------- */
/*                            Frame Render Pipeline                           */
/* -------------------------------------------------------------------------- */
static void render_screen(void)
{
    if (!s_fb || !g_panel_handle) return;

    fb_clear(C_BG);

    if (g_meter.in_wifi_server) {
        draw_wifi_server_screen();
    } else if (g_meter.in_menu) {
        draw_hold_menu();
    } else {
        draw_telemetry_screen();
    }

    esp_lcd_panel_draw_bitmap(g_panel_handle, 0, 0, LCD_H_RES, LCD_V_RES, s_fb);
}

/* -------------------------------------------------------------------------- */
/*                            Encoder & Button Task                           */
/* -------------------------------------------------------------------------- */
static void init_buttons(void)
{
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << PIN_ENC_SW) | (1ULL << PIN_ENC_A) | (1ULL << PIN_ENC_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&btn_cfg);
}

static void show_toast(const char *msg, uint32_t duration_ms)
{
    snprintf(g_meter.notification_msg, sizeof(g_meter.notification_msg), "%s", msg);
    g_meter.notification_until_ms = (esp_timer_get_time() / 1000) + duration_ms;
}

static void execute_menu_action(MenuItem_t item)
{
    switch (item) {
    case MENU_ITEM_ZERO: {
        float cur_amps = 0, sen_v = 0, temp = 0;
        read_clamp_telemetry(&cur_amps, &sen_v, &temp);
        g_meter.zero_offset_V = sen_v;
        g_meter.peak_A = 0.0f;
        g_meter.min_A = 0.0f;
        g_meter.in_menu = false;
        buzzer_confirm();
        show_toast("ZERO CALIBRATED!", 2000);
        ESP_LOGI(TAG, "Zero offset calibrated to %.3f V", sen_v);
        break;
    }
    case MENU_ITEM_RESET_PEAK:
        g_meter.peak_A = 0.0f;
        g_meter.min_A = 0.0f;
        g_meter.in_menu = false;
        buzzer_confirm();
        show_toast("PEAK CLEARED!", 1500);
        break;
    case MENU_ITEM_INTERVAL:
        if (g_meter.log_interval_s == 10) g_meter.log_interval_s = 5;
        else if (g_meter.log_interval_s == 5) g_meter.log_interval_s = 1;
        else if (g_meter.log_interval_s == 1) g_meter.log_interval_s = 30;
        else g_meter.log_interval_s = 10;
        buzzer_click();
        break;
    case MENU_ITEM_VOLUME:
        g_meter.beep_volume = (BeepVolume_t)((g_meter.beep_volume + 1) % BEEP_VOL_MAX);
        buzzer_confirm();
        break;
    case MENU_ITEM_WIFI_SERVER:
        g_meter.in_menu = false;
        start_wifi_web_server();
        buzzer_confirm();
        break;
    case MENU_ITEM_ERASE_LOGS:
        erase_all_logs();
        g_meter.in_menu = false;
        buzzer_cancel();
        show_toast("LOGS ERASED!", 1500);
        break;
    case MENU_ITEM_EXIT:
    default:
        g_meter.in_menu = false;
        buzzer_click();
        break;
    }
}

static void input_task(void *pvParam)
{
    bool last_sw = true;
    uint32_t press_start_ms = 0;
    uint8_t last_enc_state = 0;
    int enc_accum = 0;

    while (1) {
        uint32_t now_ms = esp_timer_get_time() / 1000;

        // 1. Read Rotary Encoder Knob with 4-step Detent Accumulator
        uint8_t a = gpio_get_level(PIN_ENC_A);
        uint8_t b = gpio_get_level(PIN_ENC_B);
        uint8_t enc_state = (a << 1) | b;

        if (enc_state != last_enc_state) {
            static const int8_t quad_table[16] = {
                0, -1,  1,  0,
                1,  0,  0, -1,
               -1,  0,  0,  1,
                0,  1, -1,  0
            };
            uint8_t idx = (last_enc_state << 2) | enc_state;
            enc_accum += quad_table[idx];

            if (enc_accum >= 4) {
                enc_accum = 0;
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                if (g_meter.in_menu) {
                    g_meter.menu_cursor = (g_meter.menu_cursor + 1) % MENU_ITEM_MAX;
                    g_meter.menu_last_activity_ms = now_ms;
                    buzzer_click();
                }
                xSemaphoreGive(g_mutex);
            } else if (enc_accum <= -4) {
                enc_accum = 0;
                xSemaphoreTake(g_mutex, portMAX_DELAY);
                if (g_meter.in_menu) {
                    g_meter.menu_cursor = (g_meter.menu_cursor - 1 + MENU_ITEM_MAX) % MENU_ITEM_MAX;
                    g_meter.menu_last_activity_ms = now_ms;
                    buzzer_click();
                }
                xSemaphoreGive(g_mutex);
            }
            last_enc_state = enc_state;
        }

        // 2. Read Rotary Encoder Switch
        bool sw = gpio_get_level(PIN_ENC_SW);
        if (!sw && last_sw) {
            press_start_ms = now_ms;
        } else if (sw && !last_sw) {
            uint32_t duration_ms = now_ms - press_start_ms;
            xSemaphoreTake(g_mutex, portMAX_DELAY);

            if (g_meter.in_wifi_server) {
                stop_wifi_web_server();
                buzzer_cancel();
                show_toast("WIFI STOPPED", 1500);
            } else if (duration_ms > 800) {
                g_meter.in_menu = !g_meter.in_menu;
                g_meter.menu_cursor = 0;
                g_meter.menu_last_activity_ms = now_ms;
                buzzer_confirm();
                ESP_LOGI(TAG, "Menu toggled: %d", g_meter.in_menu);
            } else if (duration_ms > 30) {
                if (g_meter.in_menu) {
                    execute_menu_action((MenuItem_t)g_meter.menu_cursor);
                    g_meter.menu_last_activity_ms = now_ms;
                } else {
                    g_meter.is_recording = !g_meter.is_recording;
                    if (g_meter.is_recording) {
                        g_meter.rec_start_time_s = esp_timer_get_time() / 1000000;
                        g_meter.rec_elapsed_s = 0;
                        g_meter.log_count = 0;
                        start_new_log_file();
                        buzzer_confirm();
                        show_toast("RECORDING STARTED", 1500);
                    } else {
                        stop_log_file();
                        buzzer_cancel();
                        show_toast("RECORDING SAVED", 1500);
                    }
                }
            }
            xSemaphoreGive(g_mutex);
        }
        last_sw = sw;

        if (g_meter.in_menu && (now_ms - g_meter.menu_last_activity_ms > 12000)) {
            xSemaphoreTake(g_mutex, portMAX_DELAY);
            g_meter.in_menu = false;
            xSemaphoreGive(g_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* -------------------------------------------------------------------------- */
/*                        Measurement & UI Refresh Task                       */
/* -------------------------------------------------------------------------- */
static void meter_task(void *pvParam)
{
    uint32_t last_log_time_s = 0;

    while (1) {
        float current = 0;
        float sensor_v = 0;
        float temp_c = 0;
        read_clamp_telemetry(&current, &sensor_v, &temp_c);

        xSemaphoreTake(g_mutex, portMAX_DELAY);
        g_meter.current_A = current;
        g_meter.sensor_voltage_V = sensor_v;
        g_meter.temp_C = temp_c;

        if (fabsf(current) > g_meter.peak_A) {
            g_meter.peak_A = fabsf(current);
        }

        uint32_t now_s = esp_timer_get_time() / 1000000;

        if (g_meter.is_recording) {
            g_meter.rec_elapsed_s = now_s - g_meter.rec_start_time_s;

            if (now_s - last_log_time_s >= g_meter.log_interval_s) {
                last_log_time_s = now_s;
                if (g_meter.log_file) {
                    fprintf(g_meter.log_file, "%lu,%.3f,%.3f,%.1f\n", 
                            g_meter.rec_elapsed_s, g_meter.current_A, g_meter.sensor_voltage_V, g_meter.temp_C);
                    fflush(g_meter.log_file);
                    g_meter.log_count++;
                    ESP_LOGI(TAG, "[LOG #%lu] %lu s: %.3f A (%.3f V, %.1f C)", 
                             g_meter.log_count, g_meter.rec_elapsed_s, g_meter.current_A, g_meter.sensor_voltage_V, g_meter.temp_C);
                }
            }
        }

        update_rgb_led_state();
        xSemaphoreGive(g_mutex);

        // Render full screen at ~20 FPS
        render_screen();

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* -------------------------------------------------------------------------- */
/*                                App Entry                                   */
/* -------------------------------------------------------------------------- */
void app_main(void)
{
    ESP_LOGI(TAG, "Starting Clamp Meter UI...");

    // Initialize LCD first to claim contiguous DMA frame buffer
    init_lcd();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    g_mutex = xSemaphoreCreateMutex();

    init_spiffs();
    init_adc();
    init_rgb_led();
    init_buzzer();
    init_temperature();
    init_buttons();

    buzzer_confirm();

    xTaskCreate(meter_task, "meter_task", 4096, NULL, 5, NULL);
    xTaskCreate(input_task, "input_task", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "Clamp Meter UI running smoothly with calibrated layout.");
}
