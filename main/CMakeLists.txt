idf_component_register(
    SRCS "main.c" "leds.c" "touch.c" "fan_select.c"
    REQUIRES hc238 esp_driver_touch_sens esp_driver_ledc esp_driver_gpio
    INCLUDE_DIRS "."
)