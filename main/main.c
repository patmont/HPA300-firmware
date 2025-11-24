#include "leds.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h" // Useful for logging messages

#define TAG "MAIN_APP"

void app_main(void) {
    ledc_init();
    ESP_LOGI(TAG, "LED Driver Initialized.");

    // Define the brightness levels using the 8-bit scale (0-255)
    const uint32_t brightness_30_percent = 77;
    const uint32_t brightness_100_percent = 255;
    const TickType_t delay_3_seconds = pdMS_TO_TICKS(3000);

    while (1) {
        // --- Step 1: Turn all LEDs ON to 30% brightness ---
        ESP_LOGI(TAG, "Setting all LEDs to 30%% brightness (%d), turning all on.", brightness_30_percent);
        leds_set_pwm_value(brightness_30_percent);
        for (int i = 0; i < NUM_LEDS; i++) {
            leds_set_led_state(i, true);
        }
        vTaskDelay(delay_3_seconds);

        // --- Step 2: Turn all LEDs to 100% brightness for 3 seconds ---
        ESP_LOGI(TAG, "Setting all active LEDs to 100%% brightness (%d).", brightness_100_percent);
        leds_set_pwm_value(brightness_100_percent);
        vTaskDelay(delay_3_seconds);

        // --- Step 3: Turn off one by one at 3 second intervals ---
        ESP_LOGI(TAG, "Turning off LEDs one by one...");
        for (int i = 0; i < NUM_LEDS; i++) {
            leds_set_led_state(i, false);
            vTaskDelay(delay_3_seconds);
        }

        // After the loop finishes, all LEDs are off. The while(1) loop starts over.
        ESP_LOGI(TAG, "All LEDs off. Starting sequence over in 3 seconds.");
        vTaskDelay(delay_3_seconds); 
    }
}
