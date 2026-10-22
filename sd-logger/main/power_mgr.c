#include "power_mgr.h"
#include "config_pins.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_pm.h"
#include "esp_log.h"

static const char *TAG = "POWER_MGR";

adc_oneshot_unit_handle_t g_adc1_handle = NULL;
static adc_cali_handle_t s_batt_cali = NULL;

// Adafruit Feather ESP32-S3 has a 2:1 resistor divider (2x 100k) on GPIO 1 (A13)
#define BATT_DIVIDER_RATIO      2.0f
#define CRITICAL_BATT_CUTOFF_V  3.25f // Safe shutdown threshold before LiPo brownout

esp_err_t power_mgr_init(void) {
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_config1, &g_adc1_handle);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Battery ADC init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    adc_oneshot_config_channel(g_adc1_handle, PIN_BATTERY_ADC, &config);

    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .chan = PIN_BATTERY_ADC,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_cali_create_scheme_curve_fitting(&cali_config, &s_batt_cali);

    ESP_LOGI(TAG, "Power Manager initialized (Cutoff: %.2fV)", CRITICAL_BATT_CUTOFF_V);
    return ESP_OK;
}

esp_err_t power_mgr_read_battery(battery_status_t *status) {
    if (!status || !g_adc1_handle) return ESP_ERR_INVALID_STATE;

    int raw = 0, voltage_mv = 0;
    adc_oneshot_read(g_adc1_handle, PIN_BATTERY_ADC, &raw);
    if (s_batt_cali) {
        adc_cali_raw_to_voltage(s_batt_cali, raw, &voltage_mv);
    } else {
        voltage_mv = (raw * 3300) / 4095;
    }

    float vbat = ((float)voltage_mv * BATT_DIVIDER_RATIO) / 1000.0f;
    status->battery_voltage_v = vbat;

    // LiPo SoC curve approximation (3.2V = 0%, 4.2V = 100%)
    if (vbat >= 4.20f) {
        status->state_of_charge_pct = 100;
    } else if (vbat <= 3.20f) {
        status->state_of_charge_pct = 0;
    } else {
        status->state_of_charge_pct = (uint8_t)(((vbat - 3.20f) / (4.20f - 3.20f)) * 100.0f);
    }

    status->is_charging = (vbat > 4.25f);
    status->is_critical_low = (vbat <= CRITICAL_BATT_CUTOFF_V && vbat > 2.0f);

    return ESP_OK;
}

void power_mgr_set_cpu_freq_low_power(bool enable_low_power) {
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = enable_low_power ? 80 : 240,
        .min_freq_mhz = 40,
        .light_sleep_enable = false,
    };
    esp_pm_configure(&pm_cfg);
    ESP_LOGI(TAG, "CPU Max Frequency set to %d MHz", enable_low_power ? 80 : 240);
#endif
}
