idf_component_register(
    SRCS "touch.c"
    REQUIRES board hc238 esp_driver_touch_sens
    INCLUDE_DIRS "include"
)