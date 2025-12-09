#ifndef fan_select_H
#define fan_select_H

#include "board.h"
#include "hc238.h"
#include "driver/gpio.h"
#include "esp_err.h"

// Function to initialize the GPIO pins for the HC238
esp_err_t fan_init(void);

esp_err_t fan_select(uint8_t fan_num);

esp_err_t fan_enable(bool en);

#endif // fan_select_H