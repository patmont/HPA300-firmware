#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    FAN_TRANSITION_PHASE_NONE = 0,
    FAN_TRANSITION_PHASE_DISABLE,
    FAN_TRANSITION_PHASE_ADDRESS,
    FAN_TRANSITION_PHASE_ENABLE,
} fan_transition_phase_t;

typedef struct {
    esp_err_t (*enable)(void *context, bool enabled);
    esp_err_t (*set_address)(void *context, uint8_t address);
    void *context;
} fan_transition_backend_t;

esp_err_t fan_transition_execute(bool turn_off, uint8_t address,
                                 const fan_transition_backend_t *backend,
                                 fan_transition_phase_t *phase);
