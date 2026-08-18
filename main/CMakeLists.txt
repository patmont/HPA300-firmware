idf_component_register(
    SRCS "ota_update.c" "ota_worker.c" "main.c" "controller.c" "diagnostics.c" "fan_control.c" "fan_select.c" "network.c" "service_health.c"
    REQUIRES app_update board bootloader_support fan_policy fan_transition_core hc238 leds touch dns_server recovery_policy esp_app_format esp_event esp_http_server esp_netif esp_timer esp_wifi espcoredump json nvs_flash spi_flash
    INCLUDE_DIRS "."
    EMBED_TXTFILES "provision.html" "update.html"
)
