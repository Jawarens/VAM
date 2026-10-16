/*
* SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
*
* SPDX-License-Identifier: MIT
*/
#include "../hal_vameter.h"
#include "../hal_config.h"
#include <mooncake.h>
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_event.h>
#include <esp_ota_ops.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_crt_bundle.h>
#include <esp_wifi.h>
extern "C"
{
#include "../utils/wifi/wifi.h"
#include "../utils/simple_ota/simple_ota.h"
}
#include <assets/assets.h>

/* -------------------------------------------------------------------------- */
/*                                Get OTA info                                */
/* -------------------------------------------------------------------------- */
static const char* _ota_server_host = "https://ota.m5stack.com";
static const char* _ota_api_check_latest = "/ota/api/v2/firmware/getFirmwareVersion";

OTA_UPGRADE::OtaInfo_t HAL_VAMeter::getLatestFirmwareInfoViaOta(OnLogPageRenderCallback_t onLogPageRender)
{
    OTA_UPGRADE::OtaInfo_t info;

    std::string url = _ota_server_host;
    url += _ota_api_check_latest;
    url += "?mac=";
    url += _get_mac();
    url += "&sku=K136&version=";
    url += version();

    spdlog::info("get ota data on: {}", url);

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 10000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        info.getInfoFailed = true;
        info.firmwareUrl = "HTTP client init failed";
        return info;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK)
    {
        esp_http_client_fetch_headers(client);
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200)
        {
            char buffer[1024] = {0};
            int read_len = esp_http_client_read_response(client, buffer, sizeof(buffer) - 1);
            esp_http_client_cleanup(client);
            if (read_len > 0)
            {
                JsonDocument doc;
                DeserializationError error = deserializeJson(doc, buffer);
                if (error == DeserializationError::Ok && doc["code"].as<int>() == 200)
                {
                    info.latestVersion = doc["data"]["version"].as<std::string>();
                    info.upgradeAvailable = doc["data"]["needUpgrade"];
                    info.firmwareUrl = doc["data"]["url"].as<std::string>();
                    info.description = doc["data"]["description"].as<std::string>();
                    info.needUsbFlashing = doc["data"]["needUsbFlashing"];
                    return info;
                }
            }
        }
        else
        {
            esp_http_client_cleanup(client);
        }
    }
    else
    {
        esp_http_client_cleanup(client);
    }

    info.getInfoFailed = true;
    info.firmwareUrl = "OTA check failed or offline";
    return info;
}

/* -------------------------------------------------------------------------- */
/*                                  OTA write                                 */
/* -------------------------------------------------------------------------- */
static OnLogPageRenderCallback_t _on_log_page_render_temp = nullptr;

bool HAL_VAMeter::upgradeFirmwareViaOta(OnLogPageRenderCallback_t onLogPageRender, const std::string& firmwareUrl)
{
    if (firmwareUrl.empty())
    {
        popWarning("Empty URL");
        return false;
    }

    if (!connectWifi(onLogPageRender, false))
        return false;

    std::string string_buffer = spdlog::fmt_lib::format(" > ota url:\n > {}\n > start ota..\n", firmwareUrl);
    onLogPageRender(string_buffer, true, false);

    // Set logging callback
    _on_log_page_render_temp = onLogPageRender;
    auto ota_on_log_callback = [](char* log) { _on_log_page_render_temp(log, true, false); };

    // Start ota
    if (simple_ota_start_via_http(firmwareUrl.c_str(), ota_on_log_callback))
    {
        popSuccess(AssetPool::GetText().Misc_Text_UpgradeDone);

        // Reboot
        spdlog::info("reboot..");
        delay(100);
        esp_restart();
        while (1)
            delay(1000);
    }

    popWarning("OTA Failed");
    return false;
}
