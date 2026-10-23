#include "config_pins.h"
#include "ina228.h"
#include "clamp_sensor.h"
#include "ds3231.h"
#include "sd_logger.h"
#include "display_gc9a01.h"
#include "power_mgr.h"

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <sys/time.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>

static const char *TAG = "MAIN";

i2c_master_bus_handle_t g_i2c_bus_handle = NULL;
system_state_t g_sys_state = {
    .active_mode = LOG_MODE_CONTINUOUS_UNTIL_DEAD,
    .timed_duration_sec = 3600,
    .elapsed_sec = 0,
    .is_logging = false,
    .sd_mounted = false,
    .ina1_online = false,
    .ina2_online = false,
    .low_battery_warning = false,
    .ui_screen = 0,
    .records_written = 0,
    .queue_overflow_count = 0,
    .is_zeroed = false,
    .ina1_zero_offset_a = 0.0f,
    .ina2_zero_offset_a = 0.0f,
    .dhab_zero_offset_a = 0.0f,
    .ho100_zero_offset_a = 0.0f,
    .zero_popup_timer = 0,
};

static void telemetry_sampler_task(void *pvParameters);
static void gui_task(void *pvParameters);
static void button_task(void *pvParameters);
static void usb_cli_task(void *pvParameters);

void app_main(void) {
    ESP_LOGI(TAG, "=== 24h+ Multi-Channel SD Telemetry Logger Starting ===");

    // -1. Immediately pull both SD CS (GPIO 11) and LCD CS (GPIO 12) HIGH to isolate shared SPI bus!
    gpio_config_t cs_init_cfg = {
        .pin_bit_mask = (1ULL << PIN_SD_CS) | (1ULL << PIN_LCD_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cs_init_cfg);
    gpio_set_level(PIN_SD_CS, 1);
    gpio_set_level(PIN_LCD_CS, 1);

    // 0. Enable Power to STEMMA QT / I2C Connector (GPIO 7)
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = (1ULL << PIN_I2C_POWER),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr_cfg);
    gpio_set_level(PIN_I2C_POWER, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    // 1. Initialize Display first & Show Wallhamn Boot Logo for 5 seconds
    display_gc9a01_init();
    display_gc9a01_show_boot_logo(5000);

    // 2. Initialize MicroSD Storage Bus
    sd_logger_init();

    // 3. Initialize Shared I2C Bus (Feather STEMMA QT: SDA=GPIO3, SCL=GPIO4)
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_PORT_NUM,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&i2c_bus_cfg, &g_i2c_bus_handle);
    if (err == ESP_OK) {
        ds3231_init();

        if (ina228_init() == ESP_OK) {
            g_sys_state.ina1_online = ina228_is_ch_online(0);
            g_sys_state.ina2_online = ina228_is_ch_online(1);
            ESP_LOGI(TAG, "INA228 Probing: INA1 (0x40)=%s, INA2 (0x41)=%s",
                     g_sys_state.ina1_online ? "ONLINE" : "OFFLINE",
                     g_sys_state.ina2_online ? "ONLINE" : "OFFLINE");
        } else {
            ESP_LOGW(TAG, "Neither INA228 responded on I2C bus");
        }
    } else {
        ESP_LOGE(TAG, "I2C master bus initialization failed: %s", esp_err_to_name(err));
    }

    // 4. Initialize Power Manager (Battery ADC)
    power_mgr_init();

    // 5. Initialize LEM Sensors (DHAB S/157 + HO-100-S)
    lem_sensors_init();
    lem_sensors_calibrate_zeros();

    // 6. Configure 3 Navigation Buttons (A1=Left, A2=Menu/Log, A3=Right)
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << PIN_BUTTON_LEFT) | (1ULL << PIN_BUTTON_MENU) | (1ULL << PIN_BUTTON_RIGHT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_cfg);

    // Launch FreeRTOS tasks
    xTaskCreatePinnedToCore(telemetry_sampler_task, "sampler", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(gui_task, "gui", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(button_task, "buttons", 5120, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(usb_cli_task, "usb_cli", 4096, NULL, 1, NULL, 0);

    ESP_LOGI(TAG, "System ready. Click A2 to Log. A1/A3 to change screen. Hold A1 (1.5s) to Zero.");
}

static void telemetry_sampler_task(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(100); // 10 Hz
    uint32_t sample_idx = 0;
    battery_status_t batt;

    while (1) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        telemetry_record_t rec = {0};
        rec.sample_idx = sample_idx++;
        
        struct timeval tv;
        gettimeofday(&tv, NULL);
        rec.timestamp_unix_ms = ((int64_t)tv.tv_sec * 1000) + (tv.tv_usec / 1000);
        ds3231_format_iso_timestamp(rec.timestamp_iso, sizeof(rec.timestamp_iso));

        // Read Battery Status
        power_mgr_read_battery(&batt);
        rec.battery_v = batt.battery_voltage_v;
        rec.battery_pct = batt.state_of_charge_pct;

        if (batt.is_critical_low && g_sys_state.is_logging) {
            ESP_LOGW(TAG, "Low battery cutoff! Auto-finalizing SD log.");
            sd_logger_stop_session();
            g_sys_state.low_battery_warning = true;
        }

        // 1. Sample INA228 #1 (0x40)
        float raw_ina1_a = 0.0f;
        if (g_sys_state.ina1_online) {
            ina228_data_t ina1;
            if (ina228_read_ch(0, &ina1) == ESP_OK) {
                rec.ina1_bus_voltage_v = ina1.bus_voltage_v;
                raw_ina1_a = ina1.current_a;
                rec.ina1_power_w = ina1.power_w;
                rec.ina1_energy_wh = ina1.energy_wh;
                rec.ina1_shunt_mv = ina1.shunt_voltage_mv;
                rec.ina1_die_temp_c = ina1.die_temp_c;
            }
        }

        // 2. Sample INA228 #2 (0x41)
        float raw_ina2_a = 0.0f;
        if (g_sys_state.ina2_online) {
            ina228_data_t ina2;
            if (ina228_read_ch(1, &ina2) == ESP_OK) {
                rec.ina2_bus_voltage_v = ina2.bus_voltage_v;
                raw_ina2_a = ina2.current_a;
                rec.ina2_power_w = ina2.power_w;
                rec.ina2_energy_wh = ina2.energy_wh;
                rec.ina2_shunt_mv = ina2.shunt_voltage_mv;
                rec.ina2_die_temp_c = ina2.die_temp_c;
            }
        }

        // 3. Sample LEM DHAB S/157 + LEM HO-100-S
        float raw_dhab_a = 0.0f, raw_ho100_a = 0.0f;
        lem_sensors_data_t lem;
        if (lem_sensors_read_all(&lem) == ESP_OK) {
            raw_dhab_a = lem.dhab_current_a;
            raw_ho100_a = lem.ho100_current_a;
            rec.dhab_ch1_50a_raw_v = lem.dhab_ch1_v;
            rec.dhab_ch2_500a_raw_v = lem.dhab_ch2_v;
            rec.dhab_in_high_range = lem.dhab_high_range;
            rec.ho100_raw_v = lem.ho100_v;
        }

        // Apply Zero / Tare Calibration Offsets
        if (g_sys_state.is_zeroed) {
            rec.ina1_current_a = raw_ina1_a - g_sys_state.ina1_zero_offset_a;
            rec.ina2_current_a = raw_ina2_a - g_sys_state.ina2_zero_offset_a;
            rec.dhab_current_a = raw_dhab_a - g_sys_state.dhab_zero_offset_a;
            rec.ho100_current_a = raw_ho100_a - g_sys_state.ho100_zero_offset_a;
        } else {
            rec.ina1_current_a = raw_ina1_a;
            rec.ina2_current_a = raw_ina2_a;
            rec.dhab_current_a = raw_dhab_a;
            rec.ho100_current_a = raw_ho100_a;
        }

        // Combined Total Metrics with EMA smoothing to eliminate noise jitter
        float raw_total_i = rec.ina1_current_a + rec.ina2_current_a + rec.dhab_current_a + rec.ho100_current_a;
        static float s_smoothed_total_i = 0.0f;
        s_smoothed_total_i = (s_smoothed_total_i * 0.75f) + (raw_total_i * 0.25f);
        rec.total_system_current_a = s_smoothed_total_i;
        float v_primary = (rec.ina1_bus_voltage_v > 0.5f) ? rec.ina1_bus_voltage_v :
                          ((rec.ina2_bus_voltage_v > 0.5f) ? rec.ina2_bus_voltage_v : 12.0f);
        rec.total_power_w = rec.ina1_power_w + rec.ina2_power_w + (v_primary * (rec.dhab_current_a + rec.ho100_current_a));

        if (g_sys_state.zero_popup_timer > 0) {
            g_sys_state.zero_popup_timer--;
        }

        // Push to SD Queue only if logging is active
        if (g_sys_state.is_logging) {
            sd_logger_push_record(&rec);
        }

        g_sys_state.latest_telemetry = rec;

        if (g_sys_state.is_logging && g_sys_state.active_mode == LOG_MODE_TIMED) {
            if (sample_idx % 10 == 0) {
                g_sys_state.elapsed_sec++;
                if (g_sys_state.elapsed_sec >= g_sys_state.timed_duration_sec) {
                    sd_logger_stop_session();
                }
            }
        }
    }
}

static void gui_task(void *pvParameters) {
    while (1) {
        display_gc9a01_update(&g_sys_state);
        // Refresh rate: 250ms when logging, 400ms when in standby (prevents visual jumping/flicker)
        vTaskDelay(pdMS_TO_TICKS(g_sys_state.is_logging ? 250 : 400));
    }
}

static void button_task(void *pvParameters) {
    int last_left = 1, last_menu = 1, last_right = 1;
    TickType_t left_press_start = 0;
    bool left_long_pressed = false;

    while (1) {
        int left  = gpio_get_level(PIN_BUTTON_LEFT);
        int menu  = gpio_get_level(PIN_BUTTON_MENU);
        int right = gpio_get_level(PIN_BUTTON_RIGHT);

        // Center / Menu Button (A2): Start / Stop Logging
        if (menu == 0 && last_menu == 1) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(PIN_BUTTON_MENU) == 0) {
                if (!g_sys_state.is_logging) {
                    if (!sd_logger_is_ready()) {
                        ESP_LOGI(TAG, "[BUTTON] Attempting SD card mount...");
                        sd_logger_init();
                    }
                    if (sd_logger_is_ready()) {
                        ESP_LOGI(TAG, "[BUTTON] Starting SD Logging session...");
                        sd_logger_start_session();
                    } else {
                        ESP_LOGW(TAG, "[BUTTON] Cannot log: SD Card not ready / mounted");
                    }
                } else {
                    ESP_LOGI(TAG, "[BUTTON] Stopping SD Logging session...");
                    sd_logger_stop_session();
                }
            }
        }

        // Left Button (A1): Short Press -> Prev Screen, Long Press (>1.5s) -> ZERO / TARE
        if (left == 0 && last_left == 1) {
            left_press_start = xTaskGetTickCount();
            left_long_pressed = false;
        } else if (left == 0 && last_left == 0) {
            if (!left_long_pressed && (xTaskGetTickCount() - left_press_start) >= pdMS_TO_TICKS(1500)) {
                left_long_pressed = true;
                if (!g_sys_state.is_zeroed) {
                    // Activate Zero / Tare
                    g_sys_state.ina1_zero_offset_a = g_sys_state.latest_telemetry.ina1_current_a + g_sys_state.ina1_zero_offset_a;
                    g_sys_state.ina2_zero_offset_a = g_sys_state.latest_telemetry.ina2_current_a + g_sys_state.ina2_zero_offset_a;
                    g_sys_state.dhab_zero_offset_a = g_sys_state.latest_telemetry.dhab_current_a + g_sys_state.dhab_zero_offset_a;
                    g_sys_state.ho100_zero_offset_a = g_sys_state.latest_telemetry.ho100_current_a + g_sys_state.ho100_zero_offset_a;
                    g_sys_state.is_zeroed = true;
                    g_sys_state.zero_popup_timer = 20; // 2 seconds
                    ESP_LOGI(TAG, "[ZERO/TARE] Set Offsets: INA1=%.3fA, INA2=%.3fA, DHAB=%.2fA, HO100=%.2fA",
                             g_sys_state.ina1_zero_offset_a, g_sys_state.ina2_zero_offset_a,
                             g_sys_state.dhab_zero_offset_a, g_sys_state.ho100_zero_offset_a);
                } else {
                    // Reset Tare
                    g_sys_state.ina1_zero_offset_a = 0.0f;
                    g_sys_state.ina2_zero_offset_a = 0.0f;
                    g_sys_state.dhab_zero_offset_a = 0.0f;
                    g_sys_state.ho100_zero_offset_a = 0.0f;
                    g_sys_state.is_zeroed = false;
                    g_sys_state.zero_popup_timer = 20;
                    ESP_LOGI(TAG, "[ZERO/TARE] Cleared to raw readings");
                }
            }
        } else if (left == 1 && last_left == 0) {
            if (!left_long_pressed) {
                // Short press: previous page
                g_sys_state.ui_screen = (g_sys_state.ui_screen + 3) % 4;
                ESP_LOGI(TAG, "[BUTTON] Left (A1) pressed -> Screen %u", g_sys_state.ui_screen);
            }
        }

        // Right Button (A3): Next UI Screen
        if (right == 0 && last_right == 1) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(PIN_BUTTON_RIGHT) == 0) {
                g_sys_state.ui_screen = (g_sys_state.ui_screen + 1) % 4;
                ESP_LOGI(TAG, "[BUTTON] Right (A3) pressed -> Screen %u", g_sys_state.ui_screen);
            }
        }

        last_left  = left;
        last_menu  = menu;
        last_right = right;

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// USB-C Serial File Bridge & Command Task
static void usb_cli_task(void *pvParameters) {
    char line[128];
    while (1) {
        if (fgets(line, sizeof(line), stdin)) {
            // Trim newline
            int l_len = strlen(line);
            while (l_len > 0 && (line[l_len - 1] == '\r' || line[l_len - 1] == '\n')) line[--l_len] = '\0';
            if (strlen(line) == 0) continue;

            if (strcmp(line, "HELP") == 0) {
                printf("=== VAM LOGGER USB COMMANDS ===\n");
                printf("DIAG               - Run low-level SD card SPI pin & command diagnostic\n");
                printf("LIST               - List all CSV log files on SD card\n");
                printf("READ <filename>    - Download/stream a CSV file over USB\n");
                printf("STATUS             - Print current telemetry snapshot\n");
                printf("ZERO               - Trigger Zero/Tare calibration\n");
                printf("UNZERO             - Clear Zero/Tare calibration\n");
                printf("===============================\n");
            } else if (strcmp(line, "LIST") == 0) {
                printf("<FILE_LIST_START>\n");
                DIR *dir = opendir("/sdcard/logs");
                if (dir) {
                    struct dirent *de;
                    while ((de = readdir(dir)) != NULL) {
                        if (de->d_name[0] != '.') {
                            char path[300];
                            snprintf(path, sizeof(path), "/sdcard/logs/%s", de->d_name);
                            FILE *f = fopen(path, "r");
                            long sz = 0;
                            if (f) {
                                fseek(f, 0, SEEK_END);
                                sz = ftell(f);
                                fclose(f);
                            }
                            printf("%s,%ld\n", de->d_name, sz);
                        }
                    }
                    closedir(dir);
                } else {
                    printf("ERROR: SD card not mounted\n");
                }
                printf("<FILE_LIST_END>\n");
            } else if (strncmp(line, "READ ", 5) == 0) {
                char *fname = line + 5;
                char path[300];
                snprintf(path, sizeof(path), "/sdcard/logs/%s", fname);
                FILE *f = fopen(path, "r");
                if (!f) {
                    printf("<ERROR: File not found: %s>\n", fname);
                } else {
                    printf("<FILE_DATA_START:%s>\n", fname);
                    char buf[256];
                    while (fgets(buf, sizeof(buf), f)) {
                        fputs(buf, stdout);
                    }
                    fclose(f);
                    printf("<FILE_DATA_END:%s>\n", fname);
                }
            } else if (strcmp(line, "STATUS") == 0) {
                printf("STATUS: Total_I=%.2fA, INA1=(%.2fV, %.3fA), INA2=(%.2fV, %.3fA), DHAB=%.2fA, HO100=%.2fA, P=%.1fW, Batt=%.2fV(%u%%), Recs=%lu, Zeroed=%d, SD=%s\n",
                       g_sys_state.latest_telemetry.total_system_current_a,
                       g_sys_state.latest_telemetry.ina1_bus_voltage_v,
                       g_sys_state.latest_telemetry.ina1_current_a,
                       g_sys_state.latest_telemetry.ina2_bus_voltage_v,
                       g_sys_state.latest_telemetry.ina2_current_a,
                       g_sys_state.latest_telemetry.dhab_current_a,
                       g_sys_state.latest_telemetry.ho100_current_a,
                       g_sys_state.latest_telemetry.total_power_w,
                       g_sys_state.latest_telemetry.battery_v,
                       g_sys_state.latest_telemetry.battery_pct,
                       (unsigned long)g_sys_state.records_written,
                       g_sys_state.is_zeroed ? 1 : 0,
                       g_sys_state.sd_mounted ? "MOUNTED" : "NO_SD");
            } else if (strcmp(line, "I2C_SCAN") == 0) {
                extern i2c_master_bus_handle_t g_i2c_bus_handle;
                printf("Scanning I2C bus...\n");
                for (uint8_t addr = 1; addr < 127; addr++) {
                    if (i2c_master_probe(g_i2c_bus_handle, addr, 50) == ESP_OK) {
                        printf(" - Found I2C device at 0x%02X\n", addr);
                    }
                }
                printf("I2C scan complete.\n");
            } else if (strcmp(line, "DIAG") == 0) {
                sd_logger_diag();
            } else if (strcmp(line, "MOUNT") == 0) {
                esp_err_t err = sd_logger_init();
                printf("MOUNT_RESULT: %s\n", esp_err_to_name(err));
            } else if (strcmp(line, "ZERO") == 0) {
                g_sys_state.ina1_zero_offset_a = g_sys_state.latest_telemetry.ina1_current_a + g_sys_state.ina1_zero_offset_a;
                g_sys_state.ina2_zero_offset_a = g_sys_state.latest_telemetry.ina2_current_a + g_sys_state.ina2_zero_offset_a;
                g_sys_state.dhab_zero_offset_a = g_sys_state.latest_telemetry.dhab_current_a + g_sys_state.dhab_zero_offset_a;
                g_sys_state.ho100_zero_offset_a = g_sys_state.latest_telemetry.ho100_current_a + g_sys_state.ho100_zero_offset_a;
                g_sys_state.is_zeroed = true;
                g_sys_state.zero_popup_timer = 20;
                printf("OK: ZERO SET\n");
            } else if (strcmp(line, "UNZERO") == 0) {
                g_sys_state.ina1_zero_offset_a = 0.0f;
                g_sys_state.ina2_zero_offset_a = 0.0f;
                g_sys_state.dhab_zero_offset_a = 0.0f;
                g_sys_state.ho100_zero_offset_a = 0.0f;
                g_sys_state.is_zeroed = false;
                g_sys_state.zero_popup_timer = 20;
                printf("OK: ZERO CLEARED\n");
            } else if (strcmp(line, "START") == 0) {
                if (!g_sys_state.is_logging) {
                    if (!sd_logger_is_ready()) {
                        sd_logger_init();
                    }
                    if (sd_logger_is_ready()) {
                        sd_logger_start_session();
                        printf("OK: LOGGING STARTED\n");
                    } else {
                        printf("ERROR: SD NOT READY\n");
                    }
                } else {
                    printf("WARN: ALREADY LOGGING\n");
                }
            } else if (strcmp(line, "STOP") == 0) {
                if (g_sys_state.is_logging) {
                    sd_logger_stop_session();
                    printf("OK: LOGGING STOPPED\n");
                } else {
                    printf("WARN: NOT LOGGING\n");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
