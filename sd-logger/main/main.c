#include "clamp_sensor.h"
#include "config_pins.h"
#include "display_gc9a01.h"
#include "ds3231.h"
#include "ina228.h"
#include "power_mgr.h"
#include "sd_logger.h"
#include "status_led.h"
#include "wifi_portal.h"


#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>


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
    .ina1_addr = INA228_I2C_ADDR_1,
    .ina2_addr = INA228_I2C_ADDR_2,
    .low_battery_warning = false,
    .battery_present = false,
    .wifi_portal_active = false,
    .usb_transfer_active = false,
    .ui_screen = 0,
    .records_written = 0,
    .queue_overflow_count = 0,
    .is_zeroed = false,
    .ina1_zero_offset_a = 0.0f,
    .ina2_zero_offset_a = 0.0f,
    .dhab_zero_offset_a = 0.0f,
    .dhab_ch1_zero_offset_a = 0.0f,
    .dhab_ch2_zero_offset_a = 0.0f,
    .ho100_zero_offset_a = 0.0f,
    .zero_popup_timer = 0,
    .dhab_invert = false,
    .ho100_invert = false,
    .invert_popup_timer = 0,
};

// Keep latest un-inverted raw readings so ZERO/TARE offsets stay accurate when
// polarity toggles
static float s_last_raw_ina1_a = 0.0f;
static float s_last_raw_ina2_a = 0.0f;
static float s_last_raw_dhab_a = 0.0f;
static float s_last_raw_dhab_ch1_a = 0.0f;
static float s_last_raw_dhab_ch2_a = 0.0f;
static float s_last_raw_ho100_a = 0.0f;

static void load_polarity_config(void) {
  nvs_handle_t h;
  if (nvs_open("vam_cfg", NVS_READONLY, &h) == ESP_OK) {
    uint8_t dhab_inv = 0, ho_inv = 0;
    if (nvs_get_u8(h, "dhab_inv", &dhab_inv) == ESP_OK) {
      g_sys_state.dhab_invert = (dhab_inv != 0);
    }
    if (nvs_get_u8(h, "ho100_inv", &ho_inv) == ESP_OK) {
      g_sys_state.ho100_invert = (ho_inv != 0);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Loaded LEM polarity from NVS: DHAB=%s, HO100=%s",
             g_sys_state.dhab_invert ? "INV" : "NORM",
             g_sys_state.ho100_invert ? "INV" : "NORM");
  }
}

static void save_polarity_config(void) {
  nvs_handle_t h;
  if (nvs_open("vam_cfg", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, "dhab_inv", g_sys_state.dhab_invert ? 1 : 0);
    nvs_set_u8(h, "ho100_inv", g_sys_state.ho100_invert ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
  }
}

void system_set_lem_invert(bool dhab_inv, bool ho100_inv) {
  g_sys_state.dhab_invert = dhab_inv;
  g_sys_state.ho100_invert = ho100_inv;
  g_sys_state.invert_popup_timer = 12;
  save_polarity_config();
  ESP_LOGI(TAG, "LEM Polarity updated: DHAB=%s, HO100=%s",
           dhab_inv ? "INV" : "NORM", ho100_inv ? "INV" : "NORM");
}

SemaphoreHandle_t g_spi_bus_mutex = NULL;

static void telemetry_sampler_task(void *pvParameters);
static void gui_task(void *pvParameters);
static void button_task(void *pvParameters);
static void usb_cli_task(void *pvParameters);

void app_main(void) {
  ESP_LOGI(TAG, "=== 24h+ Multi-Channel SD Telemetry Logger Starting ===");

  esp_err_t nvs_ret = nvs_flash_init();
  if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_ret = nvs_flash_init();
  }
  if (nvs_ret == ESP_OK) {
    load_polarity_config();
  }

  g_spi_bus_mutex = xSemaphoreCreateMutex();

  // -1. Immediately pull both SD CS (GPIO 10) and LCD CS (GPIO 12) HIGH to
  // isolate shared SPI bus!
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

  // 0. Enable Power to STEMMA QT / I2C Connector (GPIO 7) & NeoPixel Power
  // (GPIO 21)
  gpio_config_t pwr_cfg = {
      .pin_bit_mask = (1ULL << PIN_I2C_POWER) | (1ULL << PIN_NEOPIXEL_PWR),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&pwr_cfg);
  gpio_set_level(PIN_I2C_POWER, 1);
  gpio_set_level(PIN_NEOPIXEL_PWR, 1);
  vTaskDelay(pdMS_TO_TICKS(50));

  // Initialize Onboard WS2812 Status NeoPixel
  status_led_init();

  // 1. Initialize Display first & Show Wallhamn Boot Logo for 3.5 seconds
  display_gc9a01_init();
  display_gc9a01_show_boot_logo(3500);

  // 2. Initialize MicroSD Storage Bus (Adalogger CS: Pin 10)
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
      g_sys_state.ina1_addr = ina228_get_addr(0);
      g_sys_state.ina2_addr = ina228_get_addr(1);
      ESP_LOGI(TAG, "INA228 Probing: INA1 (0x%02X)=%s, INA2 (0x%02X)=%s",
               g_sys_state.ina1_addr,
               g_sys_state.ina1_online ? "ONLINE" : "OFFLINE",
               g_sys_state.ina2_addr,
               g_sys_state.ina2_online ? "ONLINE" : "OFFLINE");
    } else {
      ESP_LOGW(
          TAG,
          "Neither INA228 responded on I2C bus (will retry in background)");
    }
  } else {
    ESP_LOGE(TAG, "I2C master bus initialization failed: %s",
             esp_err_to_name(err));
  }

  // 4. Initialize Power Manager (Battery Fuel Gauge)
  power_mgr_init();

  // 5. Initialize LEM Sensors (DHAB S/157 + HO-100-S) with 32x burst
  // trimmed-mean filter
  lem_sensors_init();
  lem_sensors_calibrate_zeros();

  // 6. Configure Navigation Buttons (A0 guarded, A1=Left, A2=Menu/Log,
  // A3=Right)
  gpio_config_t btn_cfg = {
      .pin_bit_mask = (1ULL << PIN_BUTTON_A0) | (1ULL << PIN_BUTTON_LEFT) |
                      (1ULL << PIN_BUTTON_MENU) | (1ULL << PIN_BUTTON_RIGHT),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&btn_cfg);

  // Launch FreeRTOS tasks
  xTaskCreatePinnedToCore(telemetry_sampler_task, "sampler", 4096, NULL, 5,
                          NULL, 0);
  xTaskCreatePinnedToCore(gui_task, "gui", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(button_task, "buttons", 5120, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(usb_cli_task, "usb_cli", 6144, NULL, 1, NULL, 0);

  // Auto-start logging on boot if MicroSD card is mounted (fail-safe 24h+
  // recording)
  if (sd_logger_is_ready()) {
    ESP_LOGI(TAG, "MicroSD ready: Auto-starting logging session on boot...");
    sd_logger_start_session();
  } else {
    ESP_LOGW(
        TAG,
        "MicroSD not mounted at boot. Logging will begin upon card detection.");
  }

  ESP_LOGI(TAG, "System ready. Click A2: Log. Hold A2: Wi-Fi. A1/A3: Screen. "
                "Hold A1: Zero.");
}

static void telemetry_sampler_task(void *pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = pdMS_TO_TICKS(500); // 500ms (2 Hz sampling)
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
    rec.is_charging = batt.is_charging;
    g_sys_state.is_charging = batt.is_charging;
    g_sys_state.battery_present = batt.battery_present;

    if (batt.is_critical_low && g_sys_state.is_logging) {
      ESP_LOGW(TAG, "Low battery cutoff! Auto-finalizing SD log.");
      sd_logger_stop_session();
      g_sys_state.low_battery_warning = true;
    }

    // Periodic auto-retry for offline INA228 channels (every ~2 seconds)
    if ((rec.sample_idx % 4) == 0) {
      if (!g_sys_state.ina1_online && ina228_probe_and_init_ch(0) == ESP_OK) {
        g_sys_state.ina1_online = true;
        g_sys_state.ina1_addr = ina228_get_addr(0);
      }
      if (!g_sys_state.ina2_online && ina228_probe_and_init_ch(1) == ESP_OK) {
        g_sys_state.ina2_online = true;
        g_sys_state.ina2_addr = ina228_get_addr(1);
      }
    }

    // 1. Sample INA228 #1
    float raw_ina1_a = 0.0f;
    if (g_sys_state.ina1_online) {
      ina228_data_t ina1 = {0};
      if (ina228_read_ch(0, &ina1) == ESP_OK) {
        rec.ina1_bus_voltage_v = ina1.bus_voltage_v;
        raw_ina1_a = ina1.current_a;
        rec.ina1_power_w = ina1.power_w;
        rec.ina1_energy_wh = ina1.energy_wh;
        rec.ina1_shunt_mv = ina1.shunt_voltage_mv;
        rec.ina1_die_temp_c = ina1.die_temp_c;
      } else {
        g_sys_state.ina1_online = ina228_is_ch_online(0);
      }
    }

    // 2. Sample INA228 #2
    float raw_ina2_a = 0.0f;
    if (g_sys_state.ina2_online) {
      ina228_data_t ina2 = {0};
      if (ina228_read_ch(1, &ina2) == ESP_OK) {
        rec.ina2_bus_voltage_v = ina2.bus_voltage_v;
        raw_ina2_a = ina2.current_a;
        rec.ina2_power_w = ina2.power_w;
        rec.ina2_energy_wh = ina2.energy_wh;
        rec.ina2_shunt_mv = ina2.shunt_voltage_mv;
        rec.ina2_die_temp_c = ina2.die_temp_c;
      } else {
        g_sys_state.ina2_online = ina228_is_ch_online(1);
      }
    }

    // 3. Sample LEM DHAB S/157 + LEM HO-100-S (Burst filtered)
    float raw_dhab_a = 0.0f, raw_dhab_ch1_a = 0.0f, raw_dhab_ch2_high_a = 0.0f,
          raw_ho100_a = 0.0f;
    lem_sensors_data_t lem;
    if (lem_sensors_read_all(&lem) == ESP_OK) {
      raw_dhab_a = lem.dhab_current_a;
      raw_dhab_ch1_a = lem.dhab_ch1_low_a;
      raw_dhab_ch2_high_a = lem.dhab_ch2_high_a;
      raw_ho100_a = lem.ho100_current_a;
      rec.dhab_ch1_50a_raw_v = lem.dhab_ch1_v;
      rec.dhab_ch2_500a_raw_v = lem.dhab_ch2_v;
      rec.dhab_in_high_range = lem.dhab_high_range;
      rec.ho100_raw_v = lem.ho100_v;
    }

    // Save un-inverted raw values for ZERO / TARE calibration
    s_last_raw_ina1_a = raw_ina1_a;
    s_last_raw_ina2_a = raw_ina2_a;
    s_last_raw_dhab_a = raw_dhab_a;
    s_last_raw_dhab_ch1_a = raw_dhab_ch1_a;
    s_last_raw_dhab_ch2_a = raw_dhab_ch2_high_a;
    s_last_raw_ho100_a = raw_ho100_a;

    // Apply Zero / Tare Calibration Offsets in raw domain
    if (g_sys_state.is_zeroed) {
      rec.ina1_current_a = raw_ina1_a - g_sys_state.ina1_zero_offset_a;
      rec.ina2_current_a = raw_ina2_a - g_sys_state.ina2_zero_offset_a;
      rec.dhab_current_a = raw_dhab_a - g_sys_state.dhab_zero_offset_a;
      rec.dhab_ch1_low_a = raw_dhab_ch1_a - g_sys_state.dhab_ch1_zero_offset_a;
      rec.dhab_ch2_high_a =
          raw_dhab_ch2_high_a - g_sys_state.dhab_ch2_zero_offset_a;
      rec.ho100_current_a = raw_ho100_a - g_sys_state.ho100_zero_offset_a;
    } else {
      rec.ina1_current_a = raw_ina1_a;
      rec.ina2_current_a = raw_ina2_a;
      rec.dhab_current_a = raw_dhab_a;
      rec.dhab_ch1_low_a = raw_dhab_ch1_a;
      rec.dhab_ch2_high_a = raw_dhab_ch2_high_a;
      rec.ho100_current_a = raw_ho100_a;
    }

    // Apply LEM Polarity Inversion (for modules installed backwards)
    float dhab_sign = g_sys_state.dhab_invert ? -1.0f : 1.0f;
    float ho100_sign = g_sys_state.ho100_invert ? -1.0f : 1.0f;
    rec.dhab_current_a *= dhab_sign;
    rec.dhab_ch1_low_a *= dhab_sign;
    rec.dhab_ch2_high_a *= dhab_sign;
    rec.ho100_current_a *= ho100_sign;

    // Combined Total Metrics
    float raw_total_i = rec.ina1_current_a + rec.ina2_current_a +
                        rec.dhab_current_a + rec.ho100_current_a;
    static float s_smoothed_total_i = 0.0f;
    s_smoothed_total_i = (s_smoothed_total_i * 0.75f) + (raw_total_i * 0.25f);
    rec.total_system_current_a = s_smoothed_total_i;

    float v_primary =
        (rec.ina1_bus_voltage_v > 0.5f)
            ? rec.ina1_bus_voltage_v
            : ((rec.ina2_bus_voltage_v > 0.5f) ? rec.ina2_bus_voltage_v
                                               : 12.0f);
    rec.total_power_w =
        rec.ina1_power_w + rec.ina2_power_w +
        (v_primary * (rec.dhab_current_a + rec.ho100_current_a));

    if (g_sys_state.zero_popup_timer > 0) {
      g_sys_state.zero_popup_timer--;
    }
    if (g_sys_state.invert_popup_timer > 0) {
      g_sys_state.invert_popup_timer--;
    }

    // Push to SD Queue only if logging is active
    if (g_sys_state.is_logging) {
      sd_logger_push_record(&rec);
    }

    g_sys_state.latest_telemetry = rec;

    // Update Status LED state
    if (batt.is_critical_low) {
      status_led_set_state(LED_STATE_LOW_BATT);
    } else if (g_sys_state.wifi_portal_active) {
      status_led_set_state(LED_STATE_WIFI_ACTIVE);
    } else if (g_sys_state.is_logging) {
      status_led_set_state(LED_STATE_LOGGING);
    } else if (g_sys_state.is_zeroed) {
      status_led_set_state(LED_STATE_ZERO_CALIBRATED);
    } else {
      status_led_set_state(LED_STATE_IDLE);
    }
    status_led_update_tick();

    if (g_sys_state.is_logging) {
      if (sample_idx % 2 == 0) {
        g_sys_state.elapsed_sec++;
        if (g_sys_state.active_mode == LOG_MODE_TIMED &&
            g_sys_state.elapsed_sec >= g_sys_state.timed_duration_sec) {
          sd_logger_stop_session();
        }
      }
    }
  }
}

static TickType_t s_last_activity_tick = 0;

static void gui_task(void *pvParameters) {
  s_last_activity_tick = xTaskGetTickCount();
  while (1) {
    if (g_sys_state.usb_transfer_active) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    TickType_t now = xTaskGetTickCount();
    bool should_dim = (now - s_last_activity_tick) >
                      pdMS_TO_TICKS(120000); // 2 minutes timeout
    if (should_dim != g_sys_state.screen_dimmed) {
      g_sys_state.screen_dimmed = should_dim;
      display_gc9a01_set_brightness(should_dim ? 0 : 100);
    }

    if (!g_sys_state.screen_dimmed) {
      display_gc9a01_update(&g_sys_state);
      vTaskDelay(pdMS_TO_TICKS(500));
    } else {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }
}

static void button_task(void *pvParameters) {
  int last_left = 1, last_menu = 1, last_right = 1;
  TickType_t left_press_start = 0;
  TickType_t menu_press_start = 0;
  TickType_t right_press_start = 0;
  bool left_long_pressed = false;
  bool menu_long_pressed = false;
  bool right_long_pressed = false;

  while (1) {
    int left = gpio_get_level(PIN_BUTTON_LEFT);
    int menu = gpio_get_level(PIN_BUTTON_MENU);
    int right = gpio_get_level(PIN_BUTTON_RIGHT);

    if ((left == 0 && last_left == 1) || (menu == 0 && last_menu == 1) ||
        (right == 0 && last_right == 1)) {
      s_last_activity_tick = xTaskGetTickCount();
      if (g_sys_state.screen_dimmed) {
        g_sys_state.screen_dimmed = false;
        display_gc9a01_set_brightness(100);
        vTaskDelay(pdMS_TO_TICKS(300));
        last_left = left;
        last_menu = menu;
        last_right = right;
        continue;
      }
    }

    // Center / Menu Button (A2):
    // Normal Screens (0, 1, 2, 4):
    //   - Short Press: Start / Stop SD Logging
    //   - Long Press (>2.0s): Toggle Wi-Fi On/Off
    // On Wi-Fi Screen (P3):
    //   - Short Press: Toggle Wi-Fi On/Off directly!
    if (menu == 0 && last_menu == 1) {
      vTaskDelay(pdMS_TO_TICKS(50));
      if (gpio_get_level(PIN_BUTTON_MENU) == 0) {
        menu_press_start = xTaskGetTickCount();
        menu_long_pressed = false;
      }
    } else if (menu == 0 && last_menu == 0) {
      if (!menu_long_pressed &&
          (xTaskGetTickCount() - menu_press_start) >= pdMS_TO_TICKS(1500)) {
        menu_long_pressed = true;
        // Long press toggle Wi-Fi from any screen
        if (!g_sys_state.wifi_portal_active) {
          ESP_LOGI(TAG, "[BUTTON] Starting Wi-Fi Portal...");
          wifi_portal_start();
          g_sys_state.ui_screen = 3; // Switch to Wi-Fi screen
        } else {
          ESP_LOGI(TAG, "[BUTTON] Stopping Wi-Fi Portal...");
          wifi_portal_stop();
        }
      }
    } else if (menu == 1 && last_menu == 0) {
      vTaskDelay(pdMS_TO_TICKS(50));
      if (gpio_get_level(PIN_BUTTON_MENU) == 1 && !menu_long_pressed) {
        // Short press
        if (g_sys_state.ui_screen == 3) {
          // On Wi-Fi screen, short press directly toggles Wi-Fi
          if (!g_sys_state.wifi_portal_active) {
            ESP_LOGI(TAG, "[BUTTON] Toggling Wi-Fi ON from P3...");
            wifi_portal_start();
          } else {
            ESP_LOGI(TAG, "[BUTTON] Toggling Wi-Fi OFF from P3...");
            wifi_portal_stop();
          }
        } else {
          // On telemetry screens, short press starts/stops logging
          if (!g_sys_state.is_logging) {
            if (!sd_logger_is_ready()) {
              sd_logger_init();
            }
            if (sd_logger_is_ready()) {
              sd_logger_start_session();
            }
          } else {
            sd_logger_stop_session();
          }
        }
      }
    }

    // Left Button (A1): Short Press -> Prev Screen (0-4), Long Press (>1.5s) ->
    // ZERO / TARE
    if (left == 0 && last_left == 1) {
      vTaskDelay(pdMS_TO_TICKS(50));
      if (gpio_get_level(PIN_BUTTON_LEFT) == 0) {
        left_press_start = xTaskGetTickCount();
        left_long_pressed = false;
      }
    } else if (left == 0 && last_left == 0) {
      if (!left_long_pressed &&
          (xTaskGetTickCount() - left_press_start) >= pdMS_TO_TICKS(1500)) {
        left_long_pressed = true;
        if (!g_sys_state.is_zeroed) {
          g_sys_state.ina1_zero_offset_a = s_last_raw_ina1_a;
          g_sys_state.ina2_zero_offset_a = s_last_raw_ina2_a;
          g_sys_state.dhab_zero_offset_a = s_last_raw_dhab_a;
          g_sys_state.dhab_ch1_zero_offset_a = s_last_raw_dhab_ch1_a;
          g_sys_state.dhab_ch2_zero_offset_a = s_last_raw_dhab_ch2_a;
          g_sys_state.ho100_zero_offset_a = s_last_raw_ho100_a;
          g_sys_state.is_zeroed = true;
          g_sys_state.zero_popup_timer = 20;
          ESP_LOGI(TAG, "[TARE] Set Offsets");
        } else {
          g_sys_state.ina1_zero_offset_a = 0.0f;
          g_sys_state.ina2_zero_offset_a = 0.0f;
          g_sys_state.dhab_zero_offset_a = 0.0f;
          g_sys_state.dhab_ch1_zero_offset_a = 0.0f;
          g_sys_state.dhab_ch2_zero_offset_a = 0.0f;
          g_sys_state.ho100_zero_offset_a = 0.0f;
          g_sys_state.is_zeroed = false;
          g_sys_state.zero_popup_timer = 20;
          ESP_LOGI(TAG, "[TARE] Cleared");
        }
      }
    } else if (left == 1 && last_left == 0) {
      vTaskDelay(pdMS_TO_TICKS(50));
      if (gpio_get_level(PIN_BUTTON_LEFT) == 1 && !left_long_pressed) {
        g_sys_state.ui_screen = (g_sys_state.ui_screen + 4) % 5;
      }
    }

    // Right Button (A3): Short Press -> Next Screen (0..4), Long Press (>1.5s)
    // -> Cycle LEM Polarity Inversion
    if (right == 0 && last_right == 1) {
      vTaskDelay(pdMS_TO_TICKS(50));
      if (gpio_get_level(PIN_BUTTON_RIGHT) == 0) {
        right_press_start = xTaskGetTickCount();
        right_long_pressed = false;
      }
    } else if (right == 0 && last_right == 0) {
      if (!right_long_pressed &&
          (xTaskGetTickCount() - right_press_start) >= pdMS_TO_TICKS(1500)) {
        right_long_pressed = true;
        bool next_dhab = g_sys_state.dhab_invert;
        bool next_ho = g_sys_state.ho100_invert;
        if (!next_dhab && !next_ho) {
          next_dhab = true;
          next_ho = false;
        } else if (next_dhab && !next_ho) {
          next_dhab = false;
          next_ho = true;
        } else if (!next_dhab && next_ho) {
          next_dhab = true;
          next_ho = true;
        } else {
          next_dhab = false;
          next_ho = false;
        }
        system_set_lem_invert(next_dhab, next_ho);
      }
    } else if (right == 1 && last_right == 0) {
      vTaskDelay(pdMS_TO_TICKS(50));
      if (gpio_get_level(PIN_BUTTON_RIGHT) == 1 && !right_long_pressed) {
        g_sys_state.ui_screen = (g_sys_state.ui_screen + 1) % 5;
      }
    }

    last_left = left;
    last_menu = menu;
    last_right = right;

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// USB-C Serial File Bridge & Command Task
static void usb_cli_task(void *pvParameters) {
  char line[128];
  while (1) {
    if (fgets(line, sizeof(line), stdin)) {
      int l_len = strlen(line);
      while (l_len > 0 && (line[l_len - 1] == '\r' || line[l_len - 1] == '\n'))
        line[--l_len] = '\0';
      if (strlen(line) == 0)
        continue;

      if (strcmp(line, "HELP") == 0) {
        printf("=== VAM LOGGER USB COMMANDS ===\n");
        printf("DIAG               - Run SD card SPI diagnostic\n");
        printf("MOUNT              - Re-mount MicroSD card\n");
        printf("I2C_SCAN           - Scan I2C bus for INA228/RTC/FuelGauge\n");
        printf(
            "INA_RETRY          - Re-probe and initialize INA228 channels\n");
        printf("LIST               - List all CSV log files on SD card\n");
        printf("READ <filename>    - Download/stream a CSV file over USB\n");
        printf("DELETE <filename>  - Delete a CSV log file from SD card\n");
        printf("DELETE_ALL         - Delete ALL CSV log files from SD card\n");
        printf("STATUS             - Print current telemetry snapshot\n");
        printf("ZERO               - Trigger Zero/Tare calibration\n");
        printf("UNZERO             - Clear Zero/Tare calibration\n");
        printf("INVERT <DHAB|HO100|BOTH|RESET|STATUS> - Flip LEM polarity\n");
        printf("START / STOP       - Start / Stop SD logging\n");
        printf("WIFI_ON / WIFI_OFF - Toggle Wi-Fi Web Portal\n");
        printf("===============================\n");
      } else if (strcmp(line, "LIST") == 0) {
        printf("<FILE_LIST_START>\n");
        if (g_spi_bus_mutex)
          xSemaphoreTake(g_spi_bus_mutex, portMAX_DELAY);
        DIR *dir = opendir("/sdcard/logs");
        if (dir) {
          struct dirent *de;
          struct stat st;
          while ((de = readdir(dir)) != NULL) {
            if (de->d_name[0] != '.' && strstr(de->d_name, ".csv") != NULL) {
              char path[300];
              snprintf(path, sizeof(path), "/sdcard/logs/%s", de->d_name);
              long sz = 0;
              if (stat(path, &st) == 0) {
                sz = (long)st.st_size;
              }
              printf("%s,%ld\n", de->d_name, sz);
            }
          }
          closedir(dir);
        } else {
          printf("ERROR: SD card not mounted\n");
        }
        if (g_spi_bus_mutex)
          xSemaphoreGive(g_spi_bus_mutex);
        printf("<FILE_LIST_END>\n");
      } else if (strncmp(line, "READ ", 5) == 0) {
        char *fname = line + 5;
        if (strlen(fname) == 0 || strchr(fname, '/') || strchr(fname, '\\') ||
            strstr(fname, "..")) {
          printf("<ERROR: Invalid filename: %s>\n", fname);
        } else {
          char path[300];
          snprintf(path, sizeof(path), "/sdcard/logs/%s", fname);
          struct stat st;
          long fsize = 0;

          if (g_spi_bus_mutex)
            xSemaphoreTake(g_spi_bus_mutex, portMAX_DELAY);
          int stat_ok = stat(path, &st);
          if (stat_ok == 0)
            fsize = (long)st.st_size;
          FILE *f = fopen(path, "rb");
          if (g_spi_bus_mutex)
            xSemaphoreGive(g_spi_bus_mutex);

          if (!f) {
            printf("<ERROR: File not found: %s>\n", fname);
          } else {
            char *io_buf = (char *)malloc(4096);
            if (!io_buf) {
              if (g_spi_bus_mutex)
                xSemaphoreTake(g_spi_bus_mutex, portMAX_DELAY);
              fclose(f);
              if (g_spi_bus_mutex)
                xSemaphoreGive(g_spi_bus_mutex);
              printf("<ERROR: Out of memory>\n");
            } else {
              g_sys_state.usb_transfer_active = true;
              esp_log_level_set("*", ESP_LOG_NONE);
              printf("<FILE_DATA_START:%s:%ld>\n", fname, fsize);
              fflush(stdout);

              int chunks = 0;
              while (1) {
                if (g_spi_bus_mutex)
                  xSemaphoreTake(g_spi_bus_mutex, portMAX_DELAY);
                size_t n = fread(io_buf, 1, 4096, f);
                if (g_spi_bus_mutex)
                  xSemaphoreGive(g_spi_bus_mutex);
                if (n == 0)
                  break;

                fwrite(io_buf, 1, n, stdout);
                if ((++chunks % 16) == 0) {
                  fflush(stdout);
                  vTaskDelay(pdMS_TO_TICKS(1));
                }
              }
              fflush(stdout);

              if (g_spi_bus_mutex)
                xSemaphoreTake(g_spi_bus_mutex, portMAX_DELAY);
              fclose(f);
              if (g_spi_bus_mutex)
                xSemaphoreGive(g_spi_bus_mutex);
              free(io_buf);

              printf("\n<FILE_DATA_END:%s>\n", fname);
              fflush(stdout);
              esp_log_level_set("*", ESP_LOG_INFO);
              g_sys_state.usb_transfer_active = false;
            }
          }
        }
      } else if (strncmp(line, "DELETE ", 7) == 0) {
        const char *fname = line + 7;
        if (sd_logger_delete_file(fname) == ESP_OK) {
          printf("OK: DELETED %s\n", fname);
        } else {
          printf("ERROR: Failed to delete %s\n", fname);
        }
      } else if (strcmp(line, "DELETE_ALL") == 0) {
        int cnt = sd_logger_delete_all_files();
        if (cnt >= 0) {
          printf("OK: DELETED %d FILES\n", cnt);
        } else {
          printf("ERROR: SD not ready\n");
        }
      } else if (strncmp(line, "INVERT", 6) == 0) {
        if (strcmp(line, "INVERT DHAB") == 0) {
          system_set_lem_invert(!g_sys_state.dhab_invert,
                                g_sys_state.ho100_invert);
        } else if (strcmp(line, "INVERT HO100") == 0) {
          system_set_lem_invert(g_sys_state.dhab_invert,
                                !g_sys_state.ho100_invert);
        } else if (strcmp(line, "INVERT BOTH") == 0) {
          system_set_lem_invert(!g_sys_state.dhab_invert,
                                !g_sys_state.ho100_invert);
        } else if (strcmp(line, "INVERT RESET") == 0) {
          system_set_lem_invert(false, false);
        }
        printf("OK: INVERT DHAB=%d HO100=%d\n", g_sys_state.dhab_invert ? 1 : 0,
               g_sys_state.ho100_invert ? 1 : 0);
      } else if (strcmp(line, "I2C_SCAN") == 0) {
        ina228_i2c_scan();
      } else if (strcmp(line, "INA_RETRY") == 0) {
        esp_err_t e1 = ina228_probe_and_init_ch(0);
        esp_err_t e2 = ina228_probe_and_init_ch(1);
        g_sys_state.ina1_online = ina228_is_ch_online(0);
        g_sys_state.ina2_online = ina228_is_ch_online(1);
        g_sys_state.ina1_addr = ina228_get_addr(0);
        g_sys_state.ina2_addr = ina228_get_addr(1);
        printf("INA_RETRY: INA1(0x%02X)=%s(%s), INA2(0x%02X)=%s(%s)\n",
               g_sys_state.ina1_addr,
               g_sys_state.ina1_online ? "ONLINE" : "OFFLINE",
               esp_err_to_name(e1), g_sys_state.ina2_addr,
               g_sys_state.ina2_online ? "ONLINE" : "OFFLINE",
               esp_err_to_name(e2));
      } else if (strcmp(line, "DIAG") == 0) {
        sd_logger_diag();
      } else if (strcmp(line, "MOUNT") == 0) {
        esp_err_t merr = sd_logger_init();
        printf("MOUNT_RESULT: %s\n", esp_err_to_name(merr));
      } else if (strcmp(line, "STATUS") == 0) {
                char batt_txt[24];
                if (g_sys_state.battery_present && g_sys_state.latest_telemetry.battery_v >= 2.50f) {
          snprintf(batt_txt, sizeof(batt_txt), "%.2fV(%u%%)",
                   g_sys_state.latest_telemetry.battery_v,
                   g_sys_state.latest_telemetry.battery_pct);
                } else {
          snprintf(batt_txt, sizeof(batt_txt), "NO_BATT(USB)");
                }
                printf("STATUS: Total_I=%.2fA, INA1=(%.2fV, %.3fA), INA2=(%.2fV, %.3fA), INA1_ON=%d(0x%02X), INA2_ON=%d(0x%02X), DHAB_L=%.2fA, DHAB_H=%.2fA, HO100=%.2fA, Inv=[DHAB:%d,HO:%d], RawV=[D1=%.3fV, D2=%.3fV, HO=%.3fV], Batt=%s, Recs=%lu, Zeroed=%d, SD=%s, WiFi=%s\n",
                       g_sys_state.latest_telemetry.total_system_current_a,
                       g_sys_state.latest_telemetry.ina1_bus_voltage_v,
                       g_sys_state.latest_telemetry.ina1_current_a,
                       g_sys_state.latest_telemetry.ina2_bus_voltage_v,
                       g_sys_state.latest_telemetry.ina2_current_a,
                       g_sys_state.ina1_online ? 1 : 0, g_sys_state.ina1_addr,
                       g_sys_state.ina2_online ? 1 : 0, g_sys_state.ina2_addr,
                       g_sys_state.latest_telemetry.dhab_ch1_low_a,
                       g_sys_state.latest_telemetry.dhab_ch2_high_a,
                       g_sys_state.latest_telemetry.ho100_current_a,
                       g_sys_state.dhab_invert ? 1 : 0,
                       g_sys_state.ho100_invert ? 1 : 0,
                       g_sys_state.latest_telemetry.dhab_ch1_50a_raw_v,
                       g_sys_state.latest_telemetry.dhab_ch2_500a_raw_v,
                       g_sys_state.latest_telemetry.ho100_raw_v,
                       batt_txt,
                       (unsigned long)g_sys_state.records_written,
                       g_sys_state.is_zeroed ? 1 : 0,
                       g_sys_state.sd_mounted ? "MOUNTED" : "NO_SD",
                       g_sys_state.wifi_portal_active ? "ON" : "OFF");
      } else if (strcmp(line, "WIFI_ON") == 0) {
        wifi_portal_start();
        printf("OK: WIFI PORTAL STARTED (http://192.168.4.1)\n");
      } else if (strcmp(line, "WIFI_OFF") == 0) {
        wifi_portal_stop();
        printf("OK: WIFI PORTAL STOPPED\n");
      } else if (strcmp(line, "ZERO") == 0) {
        g_sys_state.ina1_zero_offset_a = s_last_raw_ina1_a;
        g_sys_state.ina2_zero_offset_a = s_last_raw_ina2_a;
        g_sys_state.dhab_zero_offset_a = s_last_raw_dhab_a;
        g_sys_state.dhab_ch1_zero_offset_a = s_last_raw_dhab_ch1_a;
        g_sys_state.dhab_ch2_zero_offset_a = s_last_raw_dhab_ch2_a;
        g_sys_state.ho100_zero_offset_a = s_last_raw_ho100_a;
        g_sys_state.is_zeroed = true;
        g_sys_state.zero_popup_timer = 20;
        printf("OK: ZERO SET\n");
      } else if (strcmp(line, "UNZERO") == 0) {
        g_sys_state.ina1_zero_offset_a = 0.0f;
        g_sys_state.ina2_zero_offset_a = 0.0f;
        g_sys_state.dhab_zero_offset_a = 0.0f;
        g_sys_state.dhab_ch1_zero_offset_a = 0.0f;
        g_sys_state.dhab_ch2_zero_offset_a = 0.0f;
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
