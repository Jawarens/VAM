/*
* SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
*
* SPDX-License-Identifier: MIT
*/
#include "config_panel.h"
#include <cstring>
#include <mooncake.h>
#include "../../../../hal/hal.h"
#include "../../triggers/triggers.h"
#include "../../../utils/system/system.h"
#include "../../../../assets/assets.h"

using namespace SmoothUIToolKit::Widgets;

static constexpr int _panel_startup_x = 360;
static constexpr int _panel_startup_y = 185;
static constexpr int _panel_startup_w = 5;
static constexpr int _panel_startup_h = 20;
static constexpr int _panel_x = 0;
static constexpr int _panel_y = 0;
static constexpr int _panel_w = 240;
static constexpr int _panel_h = 240;
static constexpr int _panel_r = 16;

void ConfigPanel::syncToSystemConfig()
{
    HAL::GetSystemConfig().recordTime = _data.config.record_time;
    HAL::GetSystemConfig().sampleIntervalMs = _data.config.sample_interval_ms;
    HAL::GetSystemConfig().triggerMode = static_cast<int>(_data.config.trigger_mode);
    HAL::GetSystemConfig().threshold = _data.config.threshold;
    HAL::SaveSystemConfig();
}

ConfigPanel::~ConfigPanel()
{
    if (_data.selector != nullptr)
        delete _data.selector;

    syncToSystemConfig();
}

void ConfigPanel::onInit()
{
    // Always restore from persistent SystemConfig (retains settings across power cycle & menu changes)
    _data.config.record_time = HAL::GetSystemConfig().recordTime;
    _data.config.sample_interval_ms = HAL::GetSystemConfig().sampleIntervalMs;
    _data.config.trigger_mode = static_cast<TriggerMode_t>(HAL::GetSystemConfig().triggerMode);
    _data.config.threshold = HAL::GetSystemConfig().threshold;

    setRenderOnUpdate(false);

    // Add configs
    addChild(new ConfigReacordTime(&_data.config));
    addChild(new ConfigTrigerMode(&_data.config));
    addChild(new ConfigThreshold(&_data.config));

    _data.selector = new ConfigSelector;
    _data.selector->init();
    _data.selector->enter(this);
    _data.selector->setRenderOnUpdate(false);
}

void ConfigPanel::popOut()
{
    SmoothWidgetBase::popOut();
    _data.selector->popOut();
}

void ConfigPanel::hide()
{
    SmoothWidgetBase::hide();
    _data.selector->hide();
    syncToSystemConfig();
}

void ConfigPanel::onPopOut()
{
    getTransition().jumpTo(_panel_startup_x, _panel_startup_y, _panel_startup_w, _panel_startup_h);
    getTransition().moveTo(_panel_x, _panel_y, _panel_w, _panel_h);
    getTransition().setDuration(300);
    // getTransition().setTransitionPath(SmoothUIToolKit::EasingPath::easeOutBack);

    getTransition().setDelay(0);
}

void ConfigPanel::onHide()
{
    constexpr int delay = 200;

    syncToSystemConfig();

    getTransition().jumpTo(_panel_x, _panel_y, _panel_w, _panel_h);
    getTransition().moveTo(_panel_startup_x, _panel_startup_y, _panel_startup_w, _panel_startup_h);
    getTransition().setDuration(300);
    // getTransition().setTransitionPath(SmoothUIToolKit::EasingPath::easeOutBack);
    getTransition().setDelay(delay);
}

void ConfigPanel::onRender()
{
    if (isHidden())
        return;

    // HAL::GetCanvas()->fillScreen(TFT_LIGHTGRAY);

    auto frame = getTransition().getValue();
    HAL::GetCanvas()->fillSmoothRoundRect(frame.x, frame.y, frame.w, frame.h, _panel_r, TFT_WHITE);

    _data.selector->render();
}

void ConfigPanel::onPostRender()
{
    // if (isHidden())
    //     return;
    // HAL::CanvasUpdate();
}

void ConfigPanel::onUpdate() { _data.selector->update(HAL::Millis()); }
