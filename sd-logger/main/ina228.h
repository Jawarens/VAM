#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INA228_I2C_ADDR_1       0x40  // Default address (A0=GND, A1=GND)
#define INA228_I2C_ADDR_2       0x41  // A0 jumper bridged to VS (A0=VS, A1=GND)

// INA228 Registers
#define INA228_REG_CONFIG       0x00
#define INA228_REG_ADC_CONFIG   0x01
#define INA228_REG_SHUNT_CAL    0x02
#define INA228_REG_SHUNT_TEMPCO 0x03
#define INA228_REG_VSHUNT       0x04
#define INA228_REG_VBUS         0x05
#define INA228_REG_DIETEMP      0x06
#define INA228_REG_CURRENT      0x07
#define INA228_REG_POWER        0x08
#define INA228_REG_ENERGY       0x09
#define INA228_REG_CHARGE       0x0A
#define INA228_REG_MANUF_ID     0x3E
#define INA228_REG_DEVICE_ID    0x3F

typedef struct {
    float bus_voltage_v;
    float shunt_voltage_mv;
    float current_a;
    float power_w;
    float energy_wh;
    float die_temp_c;
} ina228_data_t;

esp_err_t ina228_init(void);
bool ina228_is_ch_online(uint8_t ch);
esp_err_t ina228_read_ch(uint8_t ch, ina228_data_t *data);
esp_err_t ina228_read(ina228_data_t *data);
esp_err_t ina228_reset_energy(uint8_t ch);

#ifdef __cplusplus
}
#endif
