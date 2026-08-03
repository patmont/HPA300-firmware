#include "board.h"

#define ASSERT_VALID_FAN_LED(name, led_index) \
    _Static_assert((led_index) >= 0 && (led_index) < NUM_LEDS, #name " is not a valid LED index");
BOARD_FAN_LED(ASSERT_VALID_FAN_LED)
#undef ASSERT_VALID_FAN_LED

#define LED_GPIO_ENTRY(name, gpio) gpio,
const gpio_num_t led_gpios[NUM_LEDS] = {
    BOARD_LED_GPIOS(LED_GPIO_ENTRY)
};
#undef LED_GPIO_ENTRY

#define FAN_LED_MAP_ENTRY(name, led_index) led_index,
const led_index_t fan_led_map[BOARD_FAN_SPEED_COUNT] = {
    LED_OFF,
    BOARD_FAN_LED(FAN_LED_MAP_ENTRY)
};
#undef FAN_LED_MAP_ENTRY
