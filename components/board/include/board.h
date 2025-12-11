#pragma once
#include "driver/gpio.h"
#include "driver/touch_sens.h"

// Touch Key Definitions (touch pad numbers)
#define TOUCH_KEY1  TOUCH_PAD_NUM3
#define TOUCH_KEY2  TOUCH_PAD_NUM2
#define TOUCH_KEY3  TOUCH_PAD_NUM1
#define TOUCH_KEY4  TOUCH_PAD_NUM5
#define TOUCH_KEY5  TOUCH_PAD_NUM6
#define TOUCH_KEY6  TOUCH_PAD_NUM4

// LED GPIOs (simple GPIO numbers)
#define BOARD_LED_GPIOS(IDENT) \
    IDENT(LED_1,  GPIO_NUM_12) \
    IDENT(LED_2,  GPIO_NUM_15) \
    IDENT(LED_3,  GPIO_NUM_8)  \
    IDENT(LED_4,  GPIO_NUM_13) \
    IDENT(LED_5,  GPIO_NUM_7)  \
    IDENT(LED_6,  GPIO_NUM_9)  \
    IDENT(LED_7,  GPIO_NUM_14) \
    IDENT(LED_8,  GPIO_NUM_11) \
    IDENT(LED_9,  GPIO_NUM_10)

// LED Indicators
#define BOARD_FAN_LED(IDENT) \
    IDENT(FAN_LED_1,  LED_7) \
    IDENT(FAN_LED_2,  LED_4) \
    IDENT(FAN_LED_3,  LED_1) \
    IDENT(FAN_LED_4,  LED_8)

// Generate enum entries for each LED
#define LED_ENUM_ENTRY(name, gpio) name,
typedef enum {
    LED_OFF = -1,
    BOARD_LED_GPIOS(LED_ENUM_ENTRY)
    NUM_LEDS
} led_index_t;
#undef LED_ENUM_ENTRY

// Generate led_gpios array
#define LED_GPIO_ENTRY(name, gpio) gpio,
static const gpio_num_t led_gpios[NUM_LEDS] = {
    BOARD_LED_GPIOS(LED_GPIO_ENTRY)
};
#undef LED_GPIO_ENTRY

// Generate enum entries for LED fan indicators
#define FAN_LED_ENUM_ENTRY(name, led_index) name,
typedef enum {
    BOARD_FAN_LED(FAN_LED_ENUM_ENTRY)
    FAN_LED_COUNT
} fan_led_map_index_t;
#undef FAN_LED_ENUM_ENTRY

// Generated fan_led_map array
#define FAN_LED_MAP_ENTRY(name, led_index) led_index,
static const led_index_t fan_led_map[5] = {
    LED_OFF,
    BOARD_FAN_LED(FAN_LED_MAP_ENTRY)
};
#undef FAN_LED_MAP_ENTRY

// HC238 Fan Selector GPIOs
// If a select line is hardwired, set to 0 or 1.
// If MCU should control it, set to -1.
#define HC238_A0_GPIO   GPIO_NUM_NC // Hard-wired to GND
#define HC238_A0_FIXED  0

#define HC238_A1_GPIO   GPIO_NUM_18
#define HC238_A1_FIXED  -1

#define HC238_A2_GPIO   GPIO_NUM_17
#define HC238_A2_FIXED  -1

#define HC238_E1_GPIO   GPIO_NUM_NC // Hard-wired to GND
#define HC238_E1_FIXED  0

#define HC238_E2_GPIO   GPIO_NUM_NC // Hard-wired to GND
#define HC238_E2_FIXED  0

#define HC238_E3_GPIO   GPIO_NUM_16
#define HC238_E3_FIXED  -1

// Fan number definitions (matching HC238 outputs)
#define FAN_1   0   // maps to HC238 Y0
#define FAN_2   2   // maps to HC238 Y2
#define FAN_3   4   // maps to HC238 Y4
#define FAN_4   6   // maps to HC238 Y6