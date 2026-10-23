#include "sd_logger.h"
#include "config_pins.h"
#include "ds3231.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>

static const char *TAG = "SD_LOGGER";

static sdmmc_card_t *s_card = NULL;
static QueueHandle_t s_record_queue = NULL;
static FILE *s_log_file = NULL;
static bool s_is_logging = false;
static uint32_t s_records_written = 0;

#define QUEUE_CAPACITY          512
#define CHUNK_BUFFER_SIZE       8192
static char s_chunk_buffer[CHUNK_BUFFER_SIZE];
static size_t s_chunk_offset = 0;

static void sd_writer_task(void *pvParameters);

esp_err_t sd_logger_init(void) {
    esp_log_level_set("sdmmc_init", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_sd", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_common", ESP_LOG_DEBUG);
    esp_log_level_set("sdspi_host", ESP_LOG_DEBUG);
    ESP_LOGI(TAG, "Initializing SPI MicroSD card on SPI2_HOST...");

    // Create the queue and background writer task regardless of initial SD mount status
    if (!s_record_queue) {
        s_record_queue = xQueueCreate(QUEUE_CAPACITY, sizeof(telemetry_record_t));
        xTaskCreatePinnedToCore(sd_writer_task, "sd_writer", 4096, NULL, 4, NULL, 1);
    }

    // Ensure MISO, MOSI and CS have internal pull-ups enabled for SPI SD integrity
    gpio_set_pull_mode(PIN_SD_MISO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_SD_MOSI, GPIO_PULLUP_ONLY);
    gpio_config_t cs_pre = {
        .pin_bit_mask = (1ULL << PIN_SD_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cs_pre);
    gpio_set_level(PIN_SD_CS, 1);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 10000; // 10 MHz for rock-solid SPI SD transfers
    host.flags |= SDMMC_HOST_FLAG_SPI_IGNORE_DATA_CRC; // Support all 32GB/64GB SDHC/SDXC cards in SPI mode

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_SD_CS;
    slot_config.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024
    };

    esp_err_t ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "MicroSD card not mounted (%s). Ready to remount on insertion.", esp_err_to_name(ret));
        g_sys_state.sd_mounted = false;
        // CRITICAL: Ensure SD CS is driven HIGH so the SD card buffer does NOT interfere with the LCD!
        gpio_config_t cs_fix = {
            .pin_bit_mask = (1ULL << PIN_SD_CS),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        gpio_config(&cs_fix);
        gpio_set_level(PIN_SD_CS, 1);
        return ret;
    }

    g_sys_state.sd_mounted = true;
    ESP_LOGI(TAG, "SD Card mounted successfully at %s", SD_MOUNT_POINT);

    struct stat st;
    if (stat("/sdcard/logs", &st) != 0) {
        mkdir("/sdcard/logs", 0775);
    }

    return ESP_OK;
}

bool sd_logger_is_ready(void) {
    return g_sys_state.sd_mounted;
}

esp_err_t sd_logger_start_session(void) {
    if (!g_sys_state.sd_mounted) {
        if (sd_logger_init() != ESP_OK) {
            ESP_LOGW(TAG, "Cannot start log: MicroSD not ready");
            return ESP_ERR_INVALID_STATE;
        }
    }

    struct tm ti;
    ds3231_get_time(&ti);
    snprintf(g_sys_state.current_log_filename, sizeof(g_sys_state.current_log_filename),
             "/sdcard/logs/LOG_%04d%02d%02d_%02d%02d%02d.csv",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, ti.tm_hour, ti.tm_min, ti.tm_sec);

    s_log_file = fopen(g_sys_state.current_log_filename, "w");
    if (!s_log_file) {
        ESP_LOGE(TAG, "Failed to open new log file: %s", g_sys_state.current_log_filename);
        return ESP_FAIL;
    }

    // Write Multi-Channel CSV Header
    const char *header = "Timestamp_ISO,Unix_ms,Sample_Idx,INA1_V,INA1_A,INA1_W,INA2_V,INA2_A,INA2_W,DHAB_A,HO100_A,Total_Current_A,Total_Power_W,Batt_V,Batt_Pct\n";
    fwrite(header, 1, strlen(header), s_log_file);
    fflush(s_log_file);

    s_records_written = 0;
    s_chunk_offset = 0;
    s_is_logging = true;
    g_sys_state.is_logging = true;
    g_sys_state.records_written = 0;

    ESP_LOGI(TAG, "Started multi-channel telemetry log: %s", g_sys_state.current_log_filename);
    return ESP_OK;
}

esp_err_t sd_logger_stop_session(void) {
    if (!s_is_logging) return ESP_OK;

    s_is_logging = false;
    g_sys_state.is_logging = false;

    if (s_log_file && s_chunk_offset > 0) {
        fwrite(s_chunk_buffer, 1, s_chunk_offset, s_log_file);
        s_chunk_offset = 0;
    }

    if (s_log_file) {
        fflush(s_log_file);
        fsync(fileno(s_log_file));
        fclose(s_log_file);
        s_log_file = NULL;
    }

    ESP_LOGI(TAG, "Multi-channel log finalized. Total records: %lu", (unsigned long)s_records_written);
    return ESP_OK;
}

esp_err_t sd_logger_push_record(const telemetry_record_t *record) {
    if (!s_is_logging || !s_record_queue) return ESP_ERR_INVALID_STATE;

    if (xQueueSend(s_record_queue, record, 0) != pdTRUE) {
        g_sys_state.queue_overflow_count++;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void sd_writer_task(void *pvParameters) {
    telemetry_record_t rec;
    char line_buf[256];
    TickType_t last_flush_tick = xTaskGetTickCount();

    while (1) {
        if (xQueueReceive(s_record_queue, &rec, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (s_log_file && s_is_logging) {
                int len = snprintf(line_buf, sizeof(line_buf),
                                   "%s,%lld,%lu,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.2f,%u\n",
                                   rec.timestamp_iso,
                                   (long long)rec.timestamp_unix_ms,
                                   (unsigned long)rec.sample_idx,
                                   rec.ina1_bus_voltage_v,
                                   rec.ina1_current_a,
                                   rec.ina1_power_w,
                                   rec.ina2_bus_voltage_v,
                                   rec.ina2_current_a,
                                   rec.ina2_power_w,
                                   rec.dhab_current_a,
                                   rec.ho100_current_a,
                                   rec.total_system_current_a,
                                   rec.total_power_w,
                                   rec.battery_v,
                                   rec.battery_pct);

                if (len > 0) {
                    if (s_chunk_offset + len >= CHUNK_BUFFER_SIZE) {
                        fwrite(s_chunk_buffer, 1, s_chunk_offset, s_log_file);
                        s_chunk_offset = 0;
                    }
                    memcpy(&s_chunk_buffer[s_chunk_offset], line_buf, len);
                    s_chunk_offset += len;
                    s_records_written++;
                    g_sys_state.records_written = s_records_written;
                }
            }
        }

        if (s_is_logging && s_log_file) {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_flush_tick) >= pdMS_TO_TICKS(5000)) {
                if (s_chunk_offset > 0) {
                    fwrite(s_chunk_buffer, 1, s_chunk_offset, s_log_file);
                    s_chunk_offset = 0;
                }
                fflush(s_log_file);
                fsync(fileno(s_log_file));
                last_flush_tick = now;
            }
        }
    }
}

void sd_logger_diag(void) {
    ESP_LOGI(TAG, "=== SD CARD DIAGNOSTIC ===");
    esp_err_t err = sd_logger_init();
    printf("MOUNT_RESULT: %s\n", esp_err_to_name(err));
}
