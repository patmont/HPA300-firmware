idf_component_register(
    SRCS "touch.c"
    REQUIRES board esp_driver_touch_sens
    PRIV_REQUIRES freertos
    INCLUDE_DIRS "include"
)
