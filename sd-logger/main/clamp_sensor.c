#include "clamp_sensor.h"
#include "config_pins.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "LEM_SENSORS";

adc_oneshot_unit_handle_t g_adc1_handle = NULL;
static adc_cali_handle_t s_cali_dhab_ch1 = NULL;
static adc_cali_handle_t s_cali_dhab_ch2 = NULL;
static adc_cali_handle_t s_cali_ho100 = NULL;

static float s_zero_dhab_ch1_mv = 1650.0f;
static float s_zero_dhab_ch2_mv = 1650.0f;
static float s_zero_ho100_mv = 1650.0f;

#define VOLTAGE_DIVIDER_RATIO   (5.0f / 3.3f)

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

    ESP_LOGI(TAG, "LEM Sensor channels configured on ADC1");
    return ESP_OK;
}

esp_err_t lem_sensors_calibrate_zeros(void) {
    if (!g_adc1_handle) return ESP_ERR_INVALID_STATE;

    int32_t sum_dhab1 = 0, sum_dhab2 = 0, sum_ho100 = 0;
    int samples = 50;

    for (int i = 0; i < samples; i++) {
        int r1 = 0, v1 = 0;
        int r2 = 0, v2 = 0;
        int r3 = 0, v3 = 0;

        adc_oneshot_read(g_adc1_handle, PIN_DHAB_CH1_ADC, &r1);
        adc_oneshot_read(g_adc1_handle, PIN_DHAB_CH2_ADC, &r2);
        adc_oneshot_read(g_adc1_handle, PIN_HO100_ADC, &r3);

        if (s_cali_dhab_ch1) adc_cali_raw_to_voltage(s_cali_dhab_ch1, r1, &v1); else v1 = (r1 * 3300) / 4095;
        if (s_cali_dhab_ch2) adc_cali_raw_to_voltage(s_cali_dhab_ch2, r2, &v2); else v2 = (r2 * 3300) / 4095;
        if (s_cali_ho100) adc_cali_raw_to_voltage(s_cali_ho100, r3, &v3); else v3 = (r3 * 3300) / 4095;

        sum_dhab1 += v1;
        sum_dhab2 += v2;
        sum_ho100 += v3;
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    s_zero_dhab_ch1_mv = (float)sum_dhab1 / (float)samples;
    s_zero_dhab_ch2_mv = (float)sum_dhab2 / (float)samples;
    s_zero_ho100_mv = (float)sum_ho100 / (float)samples;

    ESP_LOGI(TAG, "Zero Offsets: DHAB Ch1=%.1fmV, DHAB Ch2=%.1fmV, HO100=%.1fmV",
             s_zero_dhab_ch1_mv, s_zero_dhab_ch2_mv, s_zero_ho100_mv);
    return ESP_OK;
}

esp_err_t lem_sensors_read_all(lem_sensors_data_t *data) {
    if (!data || !g_adc1_handle) return ESP_ERR_INVALID_STATE;

    int r1 = 0, v1 = 0, r2 = 0, v2 = 0, r3 = 0, v3 = 0;
    adc_oneshot_read(g_adc1_handle, PIN_DHAB_CH1_ADC, &r1);
    adc_oneshot_read(g_adc1_handle, PIN_DHAB_CH2_ADC, &r2);
    adc_oneshot_read(g_adc1_handle, PIN_HO100_ADC, &r3);

    if (s_cali_dhab_ch1) adc_cali_raw_to_voltage(s_cali_dhab_ch1, r1, &v1); else v1 = (r1 * 3300) / 4095;
    if (s_cali_dhab_ch2) adc_cali_raw_to_voltage(s_cali_dhab_ch2, r2, &v2); else v2 = (r2 * 3300) / 4095;
    if (s_cali_ho100) adc_cali_raw_to_voltage(s_cali_ho100, r3, &v3); else v3 = (r3 * 3300) / 4095;

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
    data->ho100_current_a = ((float)v3 - s_zero_ho100_mv) / sens_ho100;

    return ESP_OK;
}
