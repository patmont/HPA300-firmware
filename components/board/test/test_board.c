#include "unity.h"

#include "board.h"

TEST_CASE("board LED GPIO assignments are valid and unique", "[board]")
{
    for (int i = 0; i < NUM_LEDS; i++) {
        TEST_ASSERT_TRUE(GPIO_IS_VALID_OUTPUT_GPIO(led_gpios[i]));
        for (int j = i + 1; j < NUM_LEDS; j++) {
            TEST_ASSERT_NOT_EQUAL(led_gpios[i], led_gpios[j]);
        }
    }
}

TEST_CASE("fan LED map has one valid LED per active speed", "[board]")
{
    TEST_ASSERT_EQUAL(LED_OFF, fan_led_map[0]);
    for (int speed = 1; speed < BOARD_FAN_SPEED_COUNT; speed++) {
        TEST_ASSERT_GREATER_OR_EQUAL(0, fan_led_map[speed]);
        TEST_ASSERT_LESS_THAN(NUM_LEDS, fan_led_map[speed]);
        for (int other = speed + 1; other < BOARD_FAN_SPEED_COUNT; other++) {
            TEST_ASSERT_NOT_EQUAL(fan_led_map[speed], fan_led_map[other]);
        }
    }
}

TEST_CASE("fan outputs are valid and reachable with fixed A0", "[board]")
{
    const uint8_t outputs[] = {FAN_1, FAN_2, FAN_3, FAN_4};

    for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++) {
        TEST_ASSERT_LESS_OR_EQUAL(7, outputs[i]);
        TEST_ASSERT_EQUAL(HC238_A0_FIXED, outputs[i] & 1);
        for (size_t j = i + 1; j < sizeof(outputs) / sizeof(outputs[0]); j++) {
            TEST_ASSERT_NOT_EQUAL(outputs[i], outputs[j]);
        }
    }
}
