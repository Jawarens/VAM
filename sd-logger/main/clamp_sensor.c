#include "clamp_sensor.h"
#include "config_pins.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include <math.h>
#include <stdlib.h>

static const char *TAG = "LEM_SENSORS";

adc_oneshot_unit_handle_t g_adc1_handle = NULL;
static adc_cali_handle_t s_cali_dhab_ch1 = NULL;
static adc_cali_handle_t s_cali_dhab_ch2 = NULL;
static adc_cali_handle_t s_cali_ho100 = NULL;

static float s_zero_dhab_ch1_mv = 1650.0f;
static float s_zero_dhab_ch2_mv = 1650.0f;
static float s_zero_ho100_mv = 1650.0f;

#define VOLTAGE_DIVIDER_RATIO   (5.0f / 3.3f)

// Helper: Bubble sort for median / trimmed-mean filtering
static void sort_int(int arr[], int n) {
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (arr[j] > arr[j + 1]) {
                int tmp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = tmp;
            }
        }
    }
}

// Sample a channel with burst oversampling & trimmed-mean to reject power rail & ADC noise spikes
#define SAMPLES_PER_BURST 64
static int read_channel_filtered_mv(adc_channel_t ch, adc_cali_handle_t cali) {
    int raw_buf[SAMPLES_PER_BURST];

    for (int i = 0; i < SAMPLES_PER_BURST; i++) {
        adc_oneshot_read(g_adc1_handle, ch, &raw_buf[i]);
        esp_rom_delay_us(250); // Spaced across 16ms to reject 50Hz/60Hz AC hum and switching ripple
    }

    sort_int(raw_buf, SAMPLES_PER_BURST);

    // Trim top 25% and bottom 25% (rejects transient spikes completely, averages middle 32 samples)
    int32_t accum_raw = 0;
    int trimmed_count = 0;
    for (int i = 16; i < 48; i++) {
        accum_raw += raw_buf[i];
        trimmed_count++;
    }
    int avg_raw = accum_raw / trimmed_count;

    int voltage_mv = 0;
    if (cali) {
        adc_cali_raw_to_voltage(cali, avg_raw, &voltage_mv);
    } else {
        voltage_mv = (avg_raw * 3300) / 4095;
    }
    return voltage_mv;
}

esp_err_t lem_sensors_init(void) {
    if (!g_adc1_handle) {
        adc_oneshot_unit_init_cfg_t init_cfg = {
            .unit_id = ADC_UNIT_1,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        esp_err_t ret = adc_oneshot_new_unit(&init_cfg, &g_adc1_handle);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    adc_oneshot_chan_cfg_t ch_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    adc_oneshot_config_channel(g_adc1_handle, PIN_DHAB_CH1_ADC, &ch_cfg);
    adc_oneshot_config_channel(g_adc1_handle, PIN_DHAB_CH2_ADC, &ch_cfg);
    adc_oneshot_config_channel(g_adc1_handle, PIN_HO100_ADC, &ch_cfg);

    adc_cali_curve_fitting_config_t cali1 = { .unit_id = ADC_UNIT_1, .chan = PIN_DHAB_CH1_ADC, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_cali_create_scheme_curve_fitting(&cali1, &s_cali_dhab_ch1);

    adc_cali_curve_fitting_config_t cali2 = { .unit_id = ADC_UNIT_1, .chan = PIN_DHAB_CH2_ADC, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_cali_create_scheme_curve_fitting(&cali2, &s_cali_dhab_ch2);

    adc_cali_curve_fitting_config_t cali3 = { .unit_id = ADC_UNIT_1, .chan = PIN_HO100_ADC, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_cali_create_scheme_curve_fitting(&cali3, &s_cali_ho100);

    ESP_LOGI(TAG, "LEM Sensor channels configured on ADC1 with 32x burst trimmed-mean filter");
    return ESP_OK;
}

esp_err_t lem_sensors_calibrate_zeros(void) {
    if (!g_adc1_handle) return ESP_ERR_INVALID_STATE;

    int32_t sum_dhab1 = 0, sum_dhab2 = 0, sum_ho100 = 0;
    int passes = 30;

    for (int i = 0; i < passes; i++) {
        sum_dhab1 += read_channel_filtered_mv(PIN_DHAB_CH1_ADC, s_cali_dhab_ch1);
        vTaskDelay(pdMS_TO_TICKS(1));
        sum_dhab2 += read_channel_filtered_mv(PIN_DHAB_CH2_ADC, s_cali_dhab_ch2);
        vTaskDelay(pdMS_TO_TICKS(1));
        sum_ho100 += read_channel_filtered_mv(PIN_HO100_ADC, s_cali_ho100);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    s_zero_dhab_ch1_mv = (float)sum_dhab1 / (float)passes;
    s_zero_dhab_ch2_mv = (float)sum_dhab2 / (float)passes;
    s_zero_ho100_mv = (float)sum_ho100 / (float)passes;

    ESP_LOGI(TAG, "Zero Offsets Calibrated: DHAB Ch1=%.1fmV, DHAB Ch2=%.1fmV, HO100=%.1fmV",
             s_zero_dhab_ch1_mv, s_zero_dhab_ch2_mv, s_zero_ho100_mv);
    return ESP_OK;
}

esp_err_t lem_sensors_read_all(lem_sensors_data_t *data) {
    if (!data || !g_adc1_handle) return ESP_ERR_INVALID_STATE;

    int v1 = read_channel_filtered_mv(PIN_DHAB_CH1_ADC, s_cali_dhab_ch1);
    esp_rom_delay_us(50); // Inter-channel mux settling delay
    int v2 = read_channel_filtered_mv(PIN_DHAB_CH2_ADC, s_cali_dhab_ch2);
    esp_rom_delay_us(50);
    int v3 = read_channel_filtered_mv(PIN_HO100_ADC, s_cali_ho100);

    data->dhab_ch1_v = (float)v1 / 1000.0f;
    data->dhab_ch2_v = (float)v2 / 1000.0f;
    data->ho100_v = (float)v3 / 1000.0f;

    float sens_dhab1 = (40.0f / VOLTAGE_DIVIDER_RATIO);
    float sens_dhab2 = (4.0f / VOLTAGE_DIVIDER_RATIO);
    float curr_dhab1 = ((float)v1 - s_zero_dhab_ch1_mv) / sens_dhab1;
    float curr_dhab2 = ((float)v2 - s_zero_dhab_ch2_mv) / sens_dhab2;

    data->dhab_ch1_low_a = curr_dhab1;
    data->dhab_ch2_high_a = curr_dhab2;

    if (fabsf(curr_dhab1) <= 42.0f) {
        data->dhab_current_a = curr_dhab1;
        data->dhab_high_range = false;
    } else {
        data->dhab_current_a = curr_dhab2;
        data->dhab_high_range = true;
    }

    float sens_ho100 = (8.0f / VOLTAGE_DIVIDER_RATIO);
    float raw_ho100_curr = ((float)v3 - s_zero_ho100_mv) / sens_ho100;

    // Multi-stage IIR filter (88% prior + 12% new) to eliminate high frequency residue on 100A sensor
    static float s_filtered_ho100 = 0.0f;
    static bool s_ho100_first_run = true;
    if (s_ho100_first_run) {
        s_filtered_ho100 = raw_ho100_curr;
        s_ho100_first_run = false;
    } else {
        s_filtered_ho100 = (s_filtered_ho100 * 0.88f) + (raw_ho100_curr * 0.12f);
    }

    // Deadband noise floor suppression:
    // If measured current is within +/- 0.15A of zero, hold at exactly 0.00A
    if (fabsf(s_filtered_ho100) < 0.15f) {
        data->ho100_current_a = 0.0f;
    } else {
        data->ho100_current_a = s_filtered_ho100;
    }

    // Deadband for DHAB channels
    if (fabsf(data->dhab_ch1_low_a) < 0.05f) data->dhab_ch1_low_a = 0.0f;
    if (fabsf(data->dhab_ch2_high_a) < 0.25f) data->dhab_ch2_high_a = 0.0f;
    if (fabsf(data->dhab_current_a) < 0.05f) data->dhab_current_a = 0.0f;

    return ESP_OK;
}
