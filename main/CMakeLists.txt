idf_component_register(
    SRCS "main.c" "leds.c" "touch.c"
    REQUIRES esp_driver_touch_sens esp_driver_ledc esp_driver_gpio
    INCLUDE_DIRS "."
)