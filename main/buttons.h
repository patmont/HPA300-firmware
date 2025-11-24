#pragma once

#include <stdint.h>
#include <stdbool.h>

void buttons_init(void);
bool button_pressed(int index);   // index = 0..5