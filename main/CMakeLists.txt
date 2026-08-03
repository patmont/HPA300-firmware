idf_component_register(
    SRCS "main.c" "fan_select.c"
    REQUIRES board hc238 leds touch
    INCLUDE_DIRS "."
)
