idf_component_register(
    SRCS "leds.c"
    REQUIRES board esp_driver_gpio esp_driver_ledc
    INCLUDE_DIRS "include"
)