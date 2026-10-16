idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES fatfs sdmmc esp_lcd esp_adc spiffs driver esp_timer nvs_flash   led_strip esp_wifi esp_http_server esp_netif esp_event
)
