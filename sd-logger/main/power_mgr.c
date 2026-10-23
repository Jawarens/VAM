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
#define CRITICAL_BATT_CUTOFF_V  3.10f // Safe cutoff threshold before 18650 BMS cuts out

// 18650 Li-ion Open Circuit Voltage (OCV) State-of-Charge (SoC) Curve
static const struct {
    float v;
    float pct;
} s_18650_soc_table[] = {
    { 4.18f, 100.0f },
    { 4.08f,  90.0f },
    { 3.98f,  80.0f },
    { 3.88f,  70.0f },
    { 3.80f,  60.0f },
    { 3.73f,  50.0f },
    { 3.67f,  40.0f },
    { 3.61f,  30.0f },
    { 3.53f,  20.0f },
    { 3.40f,  10.0f },
    { 3.25f,   5.0f },
    { 3.00f,   0.0f }
};
#define SOC_TABLE_SIZE (sizeof(s_18650_soc_table) / sizeof(s_18650_soc_table[0]))

static float s_vbat_filtered = -1.0f;

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

    ESP_LOGI(TAG, "Power Manager initialized (18650 Li-ion Curve, Cutoff: %.2fV)", CRITICAL_BATT_CUTOFF_V);
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

    float raw_vbat = ((float)voltage_mv * BATT_DIVIDER_RATIO) / 1000.0f;

    // Exponential Moving Average filter (EMA) to prevent flickering digits
    if (s_vbat_filtered < 0.0f) {
        s_vbat_filtered = raw_vbat;
    } else {
        s_vbat_filtered = (s_vbat_filtered * 0.85f) + (raw_vbat * 0.15f);
    }

    float vbat = s_vbat_filtered;
    status->battery_voltage_v = vbat;

    // Check if battery is present: When no battery is plugged into JST, VBAT floats around 0.5V - 1.8V
    if (vbat < 2.50f) {
        status->state_of_charge_pct = 0;
        status->is_charging = false;
        status->is_critical_low = false;
        return ESP_OK;
    }

    // Charging detection (USB charging usually pulls 1S cell to 4.22V+)
    status->is_charging = (vbat >= 4.22f);

    // Calculate SoC % using 18650 Li-ion piece-wise linear interpolation curve
    if (vbat >= s_18650_soc_table[0].v) {
        status->state_of_charge_pct = 100;
    } else if (vbat <= s_18650_soc_table[SOC_TABLE_SIZE - 1].v) {
        status->state_of_charge_pct = 0;
    } else {
        float pct = 0.0f;
        for (int i = 0; i < SOC_TABLE_SIZE - 1; i++) {
            if (vbat <= s_18650_soc_table[i].v && vbat >= s_18650_soc_table[i + 1].v) {
                float v_high = s_18650_soc_table[i].v;
                float v_low  = s_18650_soc_table[i + 1].v;
                float p_high = s_18650_soc_table[i].pct;
                float p_low  = s_18650_soc_table[i + 1].pct;
                pct = p_low + ((vbat - v_low) / (v_high - v_low)) * (p_high - p_low);
                break;
            }
        }
        status->state_of_charge_pct = (uint8_t)(pct + 0.5f);
    }

    // Critical low check (BMS under-voltage protection alert)
    status->is_critical_low = (vbat <= CRITICAL_BATT_CUTOFF_V && vbat >= 2.50f);

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
