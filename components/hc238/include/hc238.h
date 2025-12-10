#ifndef hc238_H
#define hc238_H

#include "driver/gpio.h"
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    gpio_num_t A0_GPIO;
    gpio_num_t A1_GPIO;
    gpio_num_t A2_GPIO;

    gpio_num_t E1_GPIO;
    gpio_num_t E2_GPIO;
    gpio_num_t E3_GPIO;

    int A0_FIXED; // Structure the fixed pin values
    int A1_FIXED;
    int A2_FIXED;

    int E1_FIXED;
    int E2_FIXED;
    int E3_FIXED;

} hc238_config_t;

esp_err_t hc238_init(const hc238_config_t *cfg);
esp_err_t hc238_set_output(uint8_t index);
esp_err_t hc238_enable(bool en);

#endif // hc238_H