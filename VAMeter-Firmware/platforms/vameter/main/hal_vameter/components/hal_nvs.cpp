/*
 * SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "../hal_vameter.h"
#include "../hal_config.h"
#include <mooncake.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "nvs_handle.hpp"

void HAL_VAMeter::_nvs_init()
{
    spdlog::info("nvs init");
    _update_boot_count();
}

bool HAL_VAMeter::nvsSet(const char* key, const int32_t& value)
{
    esp_err_t err;
    std::unique_ptr<nvs::NVSHandle> handle = nvs::open_nvs_handle("sys_cfg", NVS_READWRITE, &err);
    if (err != ESP_OK)
    {
        spdlog::error("open nvs handle failed: {}", esp_err_to_name(err));
        return false;
    }

    err = handle->set_item(key, value);
    if (err != ESP_OK)
    {
        spdlog::error("nvs set {} failed", key);
        return false;
    }

    err = handle->commit();
    if (err != ESP_OK)
    {
        spdlog::error("nvs commit failed");
        return false;
    }

    spdlog::info("nvs set {} = {}", key, value);
    return true;
}

int32_t HAL_VAMeter::nvsGet(const char* key)
{
    esp_err_t err;
    std::unique_ptr<nvs::NVSHandle> handle = nvs::open_nvs_handle("sys_cfg", NVS_READWRITE, &err);
    if (err != ESP_OK)
    {
        return 0;
    }

    int32_t value = 0;
    err = handle->get_item(key, value);
    if (err == ESP_OK)
    {
        return value;
    }
    return 0;
}

void HAL_VAMeter::_update_boot_count()
{
    auto boot_count = nvsGet("boot_count");
    if (boot_count < 100000)
    {
        boot_count++;
        nvsSet("boot_count", boot_count);
    }
}

bool HAL_VAMeter::_nvs_reset()
{
    spdlog::info("reset nvs");
    return (nvs_flash_erase() == ESP_OK);
}