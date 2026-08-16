idf_component_register(
    SRCS "ota_update.c" "main.c" "controller.c" "diagnostics.c" "fan_select.c" "network.c"
    REQUIRES app_update board bootloader_support hc238 leds touch dns_server esp_app_format esp_event esp_http_server esp_netif esp_wifi json nvs_flash
    INCLUDE_DIRS "."
    EMBED_TXTFILES "provision.html" "update.html"
)
