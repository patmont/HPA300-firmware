idf_component_register(
    SRCS "main.c" "fan_select.c"
    REQUIRES esp_driver_gpio leds touch
    INCLUDE_DIRS "."
)