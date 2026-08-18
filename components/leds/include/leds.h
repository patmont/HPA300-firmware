#ifndef LEDS_H
#define LEDS_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Initialize the LEDC peripheral and all 9 GPIO pins
esp_err_t leds_init(void);

// Set the global PWM duty cycle (0 = off, max value = brightest)
esp_err_t leds_set_pwm_value(uint32_t duty_value);

// Command a specific LED (by its index 0-8) to be ON (connected to PWM) or OFF (disconnected/low)
esp_err_t leds_set_led_state(uint8_t led_index, bool enable_pwm);

#endif // LEDS_H

