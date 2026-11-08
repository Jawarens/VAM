#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_portal_start(void);
esp_err_t wifi_portal_stop(void);
bool wifi_portal_is_running(void);

#ifdef __cplusplus
}
#endif

