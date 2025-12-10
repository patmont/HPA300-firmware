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
#define LED1_GPIO  GPIO_NUM_12
#define LED2_GPIO  GPIO_NUM_15
#define LED3_GPIO  GPIO_NUM_8
#define LED4_GPIO  GPIO_NUM_13
#define LED5_GPIO  GPIO_NUM_7
#define LED6_GPIO  GPIO_NUM_9
#define LED7_GPIO  GPIO_NUM_14
#define LED8_GPIO  GPIO_NUM_11
#define LED9_GPIO  GPIO_NUM_10

typedef enum {
    LED_OFF = -1,
    LED_1 = 0,
    LED_2,
    LED_3,
    LED_4,
    LED_5,
    LED_6,
    LED_7,
    LED_8,
    LED_9,
    LED_COUNT
} led_index_t;

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