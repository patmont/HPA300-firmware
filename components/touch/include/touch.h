#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t touch_sens_init(void);
uint8_t touch_sens_get_key_num(void);
