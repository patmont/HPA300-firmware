#include "leds.h"
#include "touch.h"
#include "fan_select.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define TAG "MAIN_APP"

typedef enum {
    FAN_SPEED_OFF = 0,
    FAN_SPEED_1 = 1,
    FAN_SPEED_2 = 2,
    FAN_SPEED_3 = 3,
    FAN_SPEED_4 = 4,
    NUM_FAN_SPEEDS
} fan_speed_t;

// Map fan speeds to LED indices
static const led_index_t fan_led_map[5] = { 
    LED_OFF,
    LED_7,  // Speed 1
    LED_4,  // Speed 2
    LED_1,  // Speed 3
    LED_8   // Speed 4
};

static void update_fan_leds(fan_speed_t speed)
{
    // Turn everything off first
    for (int i = 0; i < NUM_LEDS; i++) {
        leds_set_led_state(i, false);
    }

    // If speed is valid, turn on the mapped LED
    if (speed >= FAN_SPEED_OFF && speed <= FAN_SPEED_4) {
        int led_index = fan_led_map[speed];
        leds_set_led_state(led_index, true);
        ESP_LOGI(TAG, "Fan speed = %d → LED index %d ON",
                 (int)speed, fan_led_map[speed]);
    } else {
        ESP_LOGI(TAG, "Fan speed %d is out of range; no LED updated", (int)speed);
    }
}

void app_main(void) {
    ledc_init();
    ESP_LOGI(TAG, "LED Driver Initialized.");

    touch_sens_init();
    ESP_LOGI(TAG, "Touch Sensor Initialized.");

    fan_init();
    ESP_LOGI(TAG, "Fan Selector Initialized.");
    
    // Set initial LED PWM value
    leds_set_pwm_value(255);

    // Set initial values
    uint8_t last_key = 0;
    fan_speed_t current_fan_speed = FAN_SPEED_OFF;

    fan_select(current_fan_speed);

    while (1) {

        uint8_t key = touch_sens_get_key_num();   // returns 1–6 or 0

        if (key != 0 && key != last_key) {

            // --- KEY 1: cycle fan speed in a clear, safe way ---
            if (key == 1) {
                // Use modulo arithmetic to advance and wrap cleanly.
                current_fan_speed = (fan_speed_t)((current_fan_speed + 1) % NUM_FAN_SPEEDS);

                fan_select(current_fan_speed);
                fan_enable(current_fan_speed != FAN_SPEED_OFF);
                update_fan_leds(current_fan_speed);

                ESP_LOGI(TAG, "Fan speed changed to %d", (int)current_fan_speed);
            }

        }

        last_key = key;   // debounce: only react on edges
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}