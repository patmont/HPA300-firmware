#ifndef FAN_SELECT_H
#define FAN_SELECT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

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

esp_err_t fan_select(fan_speed_t speed);

esp_err_t fan_enable(bool en);

#endif // FAN_SELECT_H
