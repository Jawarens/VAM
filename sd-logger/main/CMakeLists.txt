idf_component_register(SRCS "main.c"
                            "ina228.c"
                            "clamp_sensor.c"
                            "ds3231.c"
                            "sd_logger.c"
                            "display_gc9a01.c"
                            "power_mgr.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_timer esp_adc fatfs esp_pm sdmmc esp_driver_i2c esp_driver_gpio esp_driver_spi esp_driver_ledc esp_lcd)
