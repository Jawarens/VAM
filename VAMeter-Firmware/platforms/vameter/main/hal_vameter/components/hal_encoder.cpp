/*
* SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
*
* SPDX-License-Identifier: MIT
*/
#include "../hal_vameter.h"
#include "../hal_config.h"
#include <mooncake.h>
#include <ESP32Encoder.h>

static ESP32Encoder* _encoder = nullptr;

void HAL_VAMeter::_encoder_init()
{
    spdlog::info("encoder init");

    _encoder = new ESP32Encoder;
    _encoder->attachHalfQuad(HAL_PIN_ENCODER_A, HAL_PIN_ENCODER_B);
    _encoder->setCount(0);
}

static int _last_encoder_count = 0;
int HAL_VAMeter::getEncoderCount()
{
    // Divide by 4 for smooth 1-detent sensitivity
    int ret = static_cast<int>(_encoder->getCount()) / 4;

    // Make some noise
    if (ret != _last_encoder_count)
    {
        beep(ret > _last_encoder_count ? 3000 : 3200, 20);
        _last_encoder_count = ret;
    }

    if (_config.reverseEncoder)
        return -ret;
    return ret;
}

void HAL_VAMeter::resetEncoderCount(int value) { _encoder->setCount(value * 4); }
