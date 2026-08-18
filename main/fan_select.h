#ifndef FAN_SELECT_H
#define FAN_SELECT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "fan_transition_core.h"

typedef enum {
    FAN_SPEED_OFF,
    FAN_SPEED_1,
    FAN_SPEED_2,
    FAN_SPEED_3,
    FAN_SPEED_4,
    NUM_FAN_SPEEDS
} fan_speed_t;

// Function to initialize the GPIO pins for the HC238
esp_err_t fan_init(void);

// Performs exactly one break-before-make transaction. On error, phase is the
// operation that failed and a best-effort disable is attempted.
esp_err_t fan_transition(fan_speed_t speed, fan_transition_phase_t *phase);

#endif // FAN_SELECT_H
