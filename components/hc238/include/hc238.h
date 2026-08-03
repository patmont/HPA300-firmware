#ifndef HC238_H
#define HC238_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

typedef struct {
    gpio_num_t A0_GPIO;
    gpio_num_t A1_GPIO;
    gpio_num_t A2_GPIO;

    gpio_num_t E1_GPIO;
    gpio_num_t E2_GPIO;
    gpio_num_t E3_GPIO;

    int A0_FIXED; // -1 = controlled by GPIO, 0/1 = hard-wired level
    int A1_FIXED;
    int A2_FIXED;

    int E1_FIXED;
    int E2_FIXED;
    int E3_FIXED;

} hc238_config_t;

esp_err_t hc238_init(const hc238_config_t *cfg);
esp_err_t hc238_set_output(uint8_t index);
esp_err_t hc238_enable(bool en);

#endif // HC238_H
