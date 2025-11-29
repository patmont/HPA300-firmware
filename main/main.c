#include "leds.h"
#include "touch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h" // Useful for logging messages

#define TAG "MAIN_APP"

static bool led_state[NUM_LEDS] = {0};

void app_main(void) {
    ledc_init();
    ESP_LOGI(TAG, "LED Driver Initialized.");

    touch_sens_init();
    ESP_LOGI(TAG, "Touch Sensor Initialized.");
    
    // Set initial LED PWM value
    leds_set_pwm_value(255);

    uint8_t last_key = 0;

    while (1) {

        uint8_t key = touch_sens_get_key_num();   // returns 1–6 or 0

        if (key != 0 && key != last_key) {
            // Valid new press → toggle LED
            int idx = key - 1;   // convert 1–6 → 0–5 index

            led_state[idx] = !led_state[idx]; // toggle

            leds_set_led_state(idx, led_state[idx]);  // apply state

            ESP_LOGI(TAG, "Key %d pressed → LED %d is now %s",
                     key, idx, led_state[idx] ? "ON" : "OFF");
        }

        last_key = key;   // debounce: only react on edges

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}