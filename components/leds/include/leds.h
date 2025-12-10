#ifndef LEDS_H
#define LEDS_H

#include "driver/gpio.h"
#include <stdbool.h>
#include <stdint.h>

#define NUM_LEDS    9
#define START_GPIO  7
#define END_GPIO    15

// Initialize the LEDC peripheral and all 9 GPIO pins
void ledc_init();

// Set the global PWM duty cycle (0 = off, max value = brightest)
void leds_set_pwm_value(uint32_t duty_value);

// Command a specific LED (by its index 0-8) to be ON (connected to PWM) or OFF (disconnected/low)
void leds_set_led_state(uint8_t led_index, bool enable_pwm);

#endif // LEDS_H

