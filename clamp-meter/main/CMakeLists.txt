idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_adc spiffs driver esp_timer nvs_flash esp_driver_ledc esp_driver_tsens led_strip esp_wifi esp_http_server esp_netif esp_event
)
