/*
* SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
*
* SPDX-License-Identifier: MIT
*/
#include "../config_panel.h"
#include <cstdint>
#include <mooncake.h>
#include "../../../../../hal/hal.h"
#include "../../../triggers/triggers.h"
#include "../../../../utils/system/system.h"
#include "../../../../../assets/assets.h"
#include "core/easing_path/easing_path.h"
#include "lgfx/v1/misc/enum.hpp"
#include "spdlog/spdlog.h"

using namespace SYSTEM::INPUTS;

static constexpr int _panel_startup_x = 360;
static constexpr int _panel_startup_y = 185;
static constexpr int _panel_startup_w = 5;
static constexpr int _panel_startup_h = 20;
static constexpr int _panel_x = 9;
static constexpr int _panel_y = 8;
static constexpr int _panel_w = 222;
static constexpr int _panel_h = 44;
static constexpr int _panel_r = 11;
static uint32_t _color_container = 0xCADED5;
static uint32_t _color_primary = 0x035E36;
static uint32_t _color_on_primary = 0xFFFFFF;

void ConfigReacordTime::onInit() {}

void ConfigReacordTime::onPopOut()
{
    constexpr int delay = 0;

    getTransition().jumpTo(_panel_startup_x, _panel_startup_y, _panel_startup_w, _panel_startup_h);
    getTransition().moveTo(_panel_x, _panel_y, _panel_w, _panel_h);
    getTransition().setDuration(400);
    getTransition().setEachDelay(delay + 0, delay + 50, delay + 50, delay + 0);
}

void ConfigReacordTime::onHide()
{
    constexpr int delay = 120;

    getTransition().jumpTo(_panel_x, _panel_y, _panel_w, _panel_h);
    getTransition().moveTo(_panel_startup_x, _panel_startup_y, _panel_startup_w, _panel_startup_h);
    getTransition().setDuration(400);
    getTransition().setEachDelay(delay + 50, delay + 0, delay + 0, delay + 0);
}

void ConfigReacordTime::onRender()
{
    if (isHidden())
        return;

    auto frame = getTransition().getValue();

    // Panel
    if (isSelected())
    {
        HAL::GetCanvas()->fillSmoothRoundRect(frame.x, frame.y, frame.w, frame.h, _panel_r, _color_primary);
        HAL::GetCanvas()->setTextColor(_color_on_primary);
    }
    else
    {
        HAL::GetCanvas()->fillSmoothRoundRect(frame.x, frame.y, frame.w, frame.h, _panel_r, _color_container);
        HAL::GetCanvas()->setTextColor(_color_primary);
    }

    // Title
    AssetPool::LoadFont16(HAL::GetCanvas());
    HAL::GetCanvas()->setTextDatum(middle_left);
    HAL::GetCanvas()->drawString(AssetPool::GetText().AppWaveform_Config_RecTime, frame.x + 11, frame.y + frame.h / 2);

    // Value
    AssetPool::LoadFont24(HAL::GetCanvas());
    HAL::GetCanvas()->setTextDatum(middle_center);

    std::string string_buffer;
    if (_config->record_time >= 60 * 1000)
    {
        uint32_t mins = _config->record_time / (60 * 1000);
        uint32_t rem_s = (_config->record_time % (60 * 1000)) / 1000;
        if (rem_s > 0)
            string_buffer = std::to_string(mins) + "m" + std::to_string(rem_s) + "s";
        else
            string_buffer = std::to_string(mins) + " min";
    }
    else
    {
        string_buffer = std::to_string(_config->record_time / 1000) + " s";
    }

    HAL::GetCanvas()->drawString(string_buffer.c_str(), frame.x + 174, frame.y + frame.h / 2);
}

// 60 minutes top, 10s intervals
static constexpr uint32_t _rec_time_max = 60 * 60 * 1000; // 60 min max (3600000 ms)
static constexpr uint32_t _rec_time_min = 10 * 1000;      // 10s min
static constexpr uint32_t _rec_time_step = 10 * 1000;     // 10s step

void ConfigReacordTime::onUpdate()
{
    if (!isSelected())
        return;

    // Record time config: 10s intervals up to 60 minutes max
    if (Encoder::WasMoved())
    {
        uint32_t new_rec_time = _config->record_time;

        // Round to nearest 10s step
        if (new_rec_time % _rec_time_step != 0)
            new_rec_time = ((new_rec_time + _rec_time_step / 2) / _rec_time_step) * _rec_time_step;

        if (Encoder::GetDirection() < 0)
        {
            new_rec_time += _rec_time_step;
            if (new_rec_time > _rec_time_max)
                new_rec_time = _rec_time_max;
        }
        else
        {
            if (new_rec_time > _rec_time_step)
                new_rec_time -= _rec_time_step;
            else
                new_rec_time = _rec_time_min;

            if (new_rec_time < _rec_time_min)
                new_rec_time = _rec_time_min;
        }
        _config->record_time = new_rec_time;
    }
}

void ConfigReacordTime::onSelect()
{
}

void ConfigReacordTime::onUnSelect()
{
}