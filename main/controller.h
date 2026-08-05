#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "fan_select.h"
#include "freertos/FreeRTOS.h"

typedef enum {
    CONTROLLER_SOURCE_BOOT,
    CONTROLLER_SOURCE_TOUCH,
    CONTROLLER_SOURCE_REST,
    CONTROLLER_SOURCE_TIMER,
} controller_source_t;

typedef enum {
    CONTROLLER_NETWORK_OFFLINE,
    CONTROLLER_NETWORK_CONNECTING,
    CONTROLLER_NETWORK_PROVISIONING,
    CONTROLLER_NETWORK_CONNECTED,
} controller_network_status_t;

typedef struct {
    fan_speed_t fan_speed;
    uint8_t led_brightness_percent;
    uint8_t shutoff_mode;
    bool shutoff_active;
    controller_source_t last_change_source;
} controller_snapshot_t;

esp_err_t controller_init(void);
esp_err_t controller_handle_key(uint8_t key, TickType_t now);
esp_err_t controller_service(TickType_t now);
esp_err_t controller_set_remote_fan_speed(fan_speed_t speed);
esp_err_t controller_set_network_status(controller_network_status_t status);
esp_err_t controller_get_snapshot(controller_snapshot_t *snapshot);
const char *controller_source_name(controller_source_t source);
const char *controller_shutoff_mode_name(uint8_t mode);
