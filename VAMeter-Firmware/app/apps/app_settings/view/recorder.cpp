/*
 * SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "../app_settings.h"
#include "../../../hal/hal.h"
#include "../../utils/system/system.h"
#include "../../../assets/assets.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

using namespace SYSTEM::INPUTS;
using namespace SYSTEM::UI;
using namespace SmoothUIToolKit;
using namespace MOONCAKE::APPS;

static std::string _format_record_time(uint32_t ms)
{
    return std::to_string(ms / 1000) + " s";
}

static std::string _format_sample_interval(uint32_t ms)
{
    if (ms >= 1000)
    {
        return std::to_string(ms / 1000) + " s";
    }
    return std::to_string(ms) + " ms";
}

static std::string _format_trigger_mode(int mode)
{
    switch (mode)
    {
    case 1:
        return "V Above Level";
    case 2:
        return "V Under Level";
    case 3:
        return "A Above Level";
    case 4:
        return "A Under Level";
    case 0:
    default:
        return "Manual";
    }
}

static std::string _format_threshold(int mode, float threshold)
{
    char buf[32];
    if (mode == 1 || mode == 2)
    {
        std::snprintf(buf, sizeof(buf), "%.1f V", threshold);
        return std::string(buf);
    }
    else if (mode == 3 || mode == 4)
    {
        if (threshold < 1.0f)
            std::snprintf(buf, sizeof(buf), "%.0f mA", threshold * 1000.0f);
        else
            std::snprintf(buf, sizeof(buf), "%.2f A", threshold);
        return std::string(buf);
    }
    return "~";
}

// Page: /recorder
void AppSettings::_on_page_recorder()
{
    spdlog::info("on page recorder settings");

    int selected_index = 0;
    while (1)
    {
        auto& cfg = HAL::GetSystemConfig();

        std::vector<std::string> options;
        options.push_back("Duration: " + _format_record_time(cfg.recordTime));
        options.push_back("Interval: " + _format_sample_interval(cfg.sampleIntervalMs));
        options.push_back("Trigger: " + _format_trigger_mode(cfg.triggerMode));
        options.push_back("Threshold: " + _format_threshold(cfg.triggerMode, cfg.threshold));
        options.push_back(AssetPool::GetText().AppSettings_Option_Back);

        selected_index = SelectMenuPage::CreateAndWaitResult(
            "Recorder Settings", options, selected_index, &_data.select_page_theme);

        if (selected_index == -1 || selected_index == options.size() - 1)
            break;

        if (selected_index == 0)
            _on_page_recorder_time();
        else if (selected_index == 1)
            _on_page_recorder_interval();
        else if (selected_index == 2)
            _on_page_recorder_trigger_mode();
        else if (selected_index == 3)
            _on_page_recorder_threshold();
    }
}

// Page: /recorder/time
void AppSettings::_on_page_recorder_time()
{
    spdlog::info("on page recorder time");

    struct TimeOption
    {
        const char* label;
        uint32_t ms;
    };

    static const TimeOption kTimeOptions[] = {
        {"5 s", 5000},
        {"10 s", 10000},
        {"15 s", 15000},
        {"20 s", 20000},
        {"30 s", 30000},
        {"45 s", 45000},
        {"60 s (Max)", 60000},
    };
    constexpr size_t num_options = sizeof(kTimeOptions) / sizeof(kTimeOptions[0]);

    int selected_index = 1; // Default 10s
    uint32_t current_time = HAL::GetSystemConfig().recordTime;
    for (size_t i = 0; i < num_options; ++i)
    {
        if (kTimeOptions[i].ms == current_time)
        {
            selected_index = static_cast<int>(i);
            break;
        }
    }

    std::vector<std::string> options;
    for (size_t i = 0; i < num_options; ++i)
    {
        options.push_back(kTimeOptions[i].label);
    }
    options.push_back(AssetPool::GetText().AppSettings_Option_Back);

    int result = SelectMenuPage::CreateAndWaitResult(
        "Record Duration", options, selected_index, &_data.select_page_theme);

    if (result >= 0 && result < static_cast<int>(num_options))
    {
        HAL::GetSystemConfig().recordTime = kTimeOptions[result].ms;
        HAL::SaveSystemConfig();
    }
}

// Page: /recorder/interval
void AppSettings::_on_page_recorder_interval()
{
    spdlog::info("on page recorder interval");

    struct IntervalOption
    {
        const char* label;
        uint32_t ms;
    };

    static const IntervalOption kIntervalOptions[] = {
        {"20 ms (50 Hz)", 20},
        {"40 ms (25 Hz)", 40},
        {"50 ms (20 Hz)", 50},
        {"100 ms (10 Hz)", 100},
        {"200 ms (5 Hz)", 200},
        {"500 ms (2 Hz)", 500},
        {"1 s (1000 ms)", 1000},
    };
    constexpr size_t num_options = sizeof(kIntervalOptions) / sizeof(kIntervalOptions[0]);

    int selected_index = 4; // Default 200ms
    uint32_t current_interval = HAL::GetSystemConfig().sampleIntervalMs;
    for (size_t i = 0; i < num_options; ++i)
    {
        if (kIntervalOptions[i].ms == current_interval)
        {
            selected_index = static_cast<int>(i);
            break;
        }
    }

    std::vector<std::string> options;
    for (size_t i = 0; i < num_options; ++i)
    {
        options.push_back(kIntervalOptions[i].label);
    }
    options.push_back(AssetPool::GetText().AppSettings_Option_Back);

    int result = SelectMenuPage::CreateAndWaitResult(
        "Sample Interval", options, selected_index, &_data.select_page_theme);

    if (result >= 0 && result < static_cast<int>(num_options))
    {
        HAL::GetSystemConfig().sampleIntervalMs = kIntervalOptions[result].ms;
        HAL::SaveSystemConfig();
    }
}

// Page: /recorder/trigger_mode
void AppSettings::_on_page_recorder_trigger_mode()
{
    spdlog::info("on page recorder trigger mode");

    std::vector<std::string> options = {
        "Manual",
        "V Above Level (VHL)",
        "V Under Level (VLL)",
        "A Above Level (CHL)",
        "A Under Level (CLL)",
    };
    options.push_back(AssetPool::GetText().AppSettings_Option_Back);

    int current_mode = HAL::GetSystemConfig().triggerMode;
    if (current_mode < 0 || current_mode > 4)
        current_mode = 0;

    int result = SelectMenuPage::CreateAndWaitResult(
        "Trigger Mode", options, current_mode, &_data.select_page_theme);

    if (result >= 0 && result <= 4)
    {
        HAL::GetSystemConfig().triggerMode = result;
        // Default threshold if invalid for mode
        if (result == 1 || result == 2)
        {
            if (HAL::GetSystemConfig().threshold <= 0.0f || HAL::GetSystemConfig().threshold > 36.0f)
                HAL::GetSystemConfig().threshold = 1.0f;
        }
        else if (result == 3 || result == 4)
        {
            if (HAL::GetSystemConfig().threshold <= 0.0f || HAL::GetSystemConfig().threshold > 10.0f)
                HAL::GetSystemConfig().threshold = 0.3f;
        }
        HAL::SaveSystemConfig();
    }
}

// Page: /recorder/threshold
void AppSettings::_on_page_recorder_threshold()
{
    spdlog::info("on page recorder threshold");

    int mode = HAL::GetSystemConfig().triggerMode;
    std::vector<std::string> options;
    std::vector<float> values;

    if (mode == 3 || mode == 4)
    {
        // Current thresholds
        struct CurrOpt { const char* label; float val; };
        static const CurrOpt kCurrOpts[] = {
            {"10 mA", 0.010f},
            {"50 mA", 0.050f},
            {"100 mA", 0.100f},
            {"200 mA", 0.200f},
            {"300 mA", 0.300f},
            {"500 mA", 0.500f},
            {"1.0 A", 1.000f},
            {"2.0 A", 2.000f},
        };
        for (const auto& opt : kCurrOpts)
        {
            options.push_back(opt.label);
            values.push_back(opt.val);
        }
    }
    else
    {
        // Voltage thresholds (default / voltage modes)
        struct VoltOpt { const char* label; float val; };
        static const VoltOpt kVoltOpts[] = {
            {"0.5 V", 0.5f},
            {"1.0 V", 1.0f},
            {"2.0 V", 2.0f},
            {"3.3 V", 3.3f},
            {"5.0 V", 5.0f},
            {"9.0 V", 9.0f},
            {"12.0 V", 12.0f},
            {"20.0 V", 20.0f},
        };
        for (const auto& opt : kVoltOpts)
        {
            options.push_back(opt.label);
            values.push_back(opt.val);
        }
    }
    options.push_back(AssetPool::GetText().AppSettings_Option_Back);

    int selected_index = 1;
    float current_thresh = HAL::GetSystemConfig().threshold;
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (std::abs(values[i] - current_thresh) < 0.001f)
        {
            selected_index = static_cast<int>(i);
            break;
        }
    }

    int result = SelectMenuPage::CreateAndWaitResult(
        "Trigger Threshold", options, selected_index, &_data.select_page_theme);

    if (result >= 0 && result < static_cast<int>(values.size()))
    {
        HAL::GetSystemConfig().threshold = values[result];
        HAL::SaveSystemConfig();
    }
}
