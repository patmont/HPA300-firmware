#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "fan_control.h"
#include "freertos/FreeRTOS.h"

typedef fan_control_source_t controller_source_t;
#define CONTROLLER_SOURCE_BOOT FAN_CONTROL_SOURCE_BOOT
#define CONTROLLER_SOURCE_TOUCH FAN_CONTROL_SOURCE_TOUCH
#define CONTROLLER_SOURCE_REST FAN_CONTROL_SOURCE_REST
#define CONTROLLER_SOURCE_TIMER FAN_CONTROL_SOURCE_TIMER

typedef enum {
    CONTROLLER_NETWORK_OFFLINE,
    CONTROLLER_NETWORK_CONNECTING,
    CONTROLLER_NETWORK_PROVISIONING,
    CONTROLLER_NETWORK_CONNECTED,
    CONTROLLER_NETWORK_UPDATING,
} controller_network_status_t;

typedef struct {
    fan_speed_t fan_speed;
    fan_speed_t desired_speed;
    uint8_t led_brightness_percent;
    uint8_t shutoff_mode;
    bool shutoff_active;
    bool pending;
    bool fault_latched;
    bool maintenance_active;
    uint32_t accepted_sequence;
    uint32_t applied_sequence;
    controller_source_t last_change_source;
    fan_control_state_t control_state;
} controller_snapshot_t;

esp_err_t controller_init(void);
void controller_set_led_available(bool available);
esp_err_t controller_handle_key(uint8_t key, TickType_t now);
esp_err_t controller_service(TickType_t now);
esp_err_t controller_set_remote_fan_speed(fan_speed_t speed, uint32_t *sequence);
esp_err_t controller_set_network_status(controller_network_status_t status);
esp_err_t controller_get_snapshot(controller_snapshot_t *snapshot);
const char *controller_source_name(controller_source_t source);
const char *controller_shutoff_mode_name(uint8_t mode);
