#include "leds.h"
#include "touch.h"
#include "board.h"
#include "fan_select.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"

#define TAG "MAIN_APP"

#define TOUCH_DEBOUNCE_MS 150
#define LED_WAKE_DURATION_MS 5000

typedef enum {
    LED_BRIGHTNESS_HIGH,
    LED_BRIGHTNESS_MEDIUM,
    LED_BRIGHTNESS_LOW,
    LED_BRIGHTNESS_OFF,
    NUM_LED_BRIGHTNESS_LEVELS
} led_brightness_t;

static const uint8_t s_led_brightness_duty[NUM_LED_BRIGHTNESS_LEVELS] = {
    [LED_BRIGHTNESS_HIGH] = 255,   // 100%
    [LED_BRIGHTNESS_MEDIUM] = 128, // 50%, rounded to the nearest 8-bit duty
    [LED_BRIGHTNESS_LOW] = 13,     // 5%, rounded to the nearest 8-bit duty
    [LED_BRIGHTNESS_OFF] = 0,
};

typedef struct {
    fan_speed_t fan_speed;
    led_brightness_t led_brightness; // User-selected, intended brightness
    TickType_t last_key_tick[6];
    bool has_key_tick[6];
    TickType_t last_non_dimmer_touch_tick;
    bool brightness_wake_active;
} controller_state_t;

static esp_err_t controller_set_led_brightness(controller_state_t *state, led_brightness_t brightness);

static esp_err_t update_fan_leds(fan_speed_t speed)
{
    ESP_RETURN_ON_FALSE(speed >= FAN_SPEED_OFF && speed < NUM_FAN_SPEEDS,
                        ESP_ERR_INVALID_ARG, TAG, "invalid fan speed %d", (int)speed);

    // Turn everything off first
    for (int i = 0; i < NUM_LEDS; i++) {
        ESP_RETURN_ON_ERROR(leds_set_led_state(i, false), TAG, "failed to turn off LED %d", i);
    }

    if (speed != FAN_SPEED_OFF) {
        led_index_t led_index = fan_led_map[speed];
        ESP_RETURN_ON_ERROR(leds_set_led_state((uint8_t)led_index, true), TAG,
                            "failed to turn on fan LED");
        ESP_LOGI(TAG, "Fan speed = %d -> LED index %d ON", (int)speed, (int)led_index);
    }

    return ESP_OK;
}

static esp_err_t controller_set_fan_speed(controller_state_t *state, fan_speed_t speed)
{
    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_INVALID_ARG, TAG, "controller state is NULL");
    ESP_RETURN_ON_FALSE(speed >= FAN_SPEED_OFF && speed < NUM_FAN_SPEEDS,
                        ESP_ERR_INVALID_ARG, TAG, "invalid fan speed %d", (int)speed);

    bool crosses_off = state->fan_speed != speed &&
                       (state->fan_speed == FAN_SPEED_OFF || speed == FAN_SPEED_OFF);

    ESP_RETURN_ON_ERROR(fan_select(speed), TAG, "failed to select fan speed %d", (int)speed);
    state->fan_speed = speed;

    esp_err_t err = update_fan_leds(speed);
    if (err != ESP_OK) {
        // A display failure must not leave an unrepresented active fan state.
        fan_select(FAN_SPEED_OFF);
        state->fan_speed = FAN_SPEED_OFF;
        update_fan_leds(FAN_SPEED_OFF);
        return err;
    }

    if (crosses_off) {
        // Clear or replace the LED routing before raising PWM. In particular,
        // this prevents the previous speed indicator from flashing during an
        // active-to-off transition.
        err = controller_set_led_brightness(state, LED_BRIGHTNESS_HIGH);
        if (err != ESP_OK) {
            fan_select(FAN_SPEED_OFF);
            state->fan_speed = FAN_SPEED_OFF;
            update_fan_leds(FAN_SPEED_OFF);
            return err;
        }
    }

    return ESP_OK;
}

static esp_err_t controller_apply_led_brightness(led_brightness_t brightness)
{
    ESP_RETURN_ON_FALSE(brightness >= LED_BRIGHTNESS_HIGH && brightness < NUM_LED_BRIGHTNESS_LEVELS,
                        ESP_ERR_INVALID_ARG, TAG, "invalid LED brightness %d", (int)brightness);

    ESP_RETURN_ON_ERROR(leds_set_pwm_value(s_led_brightness_duty[brightness]), TAG,
                        "failed to set LED brightness");
    ESP_LOGI(TAG, "LED brightness set to %u%%", (unsigned)(s_led_brightness_duty[brightness] * 100U / 255U));
    return ESP_OK;
}

static esp_err_t controller_set_led_brightness(controller_state_t *state, led_brightness_t brightness)
{
    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_INVALID_ARG, TAG, "controller state is NULL");
    ESP_RETURN_ON_FALSE(brightness >= LED_BRIGHTNESS_HIGH && brightness < NUM_LED_BRIGHTNESS_LEVELS,
                        ESP_ERR_INVALID_ARG, TAG, "invalid LED brightness %d", (int)brightness);

    // Key 3 changes the intended brightness immediately and must cancel any
    // temporary wake initiated by another key.
    state->led_brightness = brightness;
    state->brightness_wake_active = false;
    return controller_apply_led_brightness(brightness);
}

static esp_err_t controller_wake_led_brightness(controller_state_t *state, TickType_t now)
{
    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_INVALID_ARG, TAG, "controller state is NULL");

    if (state->led_brightness == LED_BRIGHTNESS_HIGH) {
        return ESP_OK;
    }

    state->last_non_dimmer_touch_tick = now;
    state->brightness_wake_active = true;
    return controller_apply_led_brightness(LED_BRIGHTNESS_HIGH);
}

static esp_err_t controller_service_led_wake(controller_state_t *state, TickType_t now)
{
    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_INVALID_ARG, TAG, "controller state is NULL");

    if (!state->brightness_wake_active ||
        (now - state->last_non_dimmer_touch_tick) < pdMS_TO_TICKS(LED_WAKE_DURATION_MS)) {
        return ESP_OK;
    }

    state->brightness_wake_active = false;
    return controller_apply_led_brightness(state->led_brightness);
}

static fan_speed_t controller_next_fan_speed(fan_speed_t current_speed, uint8_t key)
{
    switch (key) {
        case 1:
            switch (current_speed) {
                case FAN_SPEED_OFF:
                    return FAN_SPEED_1;
                case FAN_SPEED_1:
                    return FAN_SPEED_2;
                case FAN_SPEED_2:
                    return FAN_SPEED_3;
                case FAN_SPEED_3:
                case FAN_SPEED_4:
                default:
                    return FAN_SPEED_OFF;
            }

        case 2:
            return current_speed == FAN_SPEED_4 ? FAN_SPEED_OFF : FAN_SPEED_4;

        default:
            return current_speed;
    }
}

static esp_err_t controller_handle_key(controller_state_t *state, uint8_t key, TickType_t now)
{
    if (key < 1 || key > 6) {
        return ESP_OK;
    }

    uint8_t key_index = key - 1;
    if (state->has_key_tick[key_index] &&
        (now - state->last_key_tick[key_index]) < pdMS_TO_TICKS(TOUCH_DEBOUNCE_MS)) {
        ESP_LOGD(TAG, "Ignored control key within debounce interval");
        return ESP_OK;
    }

    state->last_key_tick[key_index] = now;
    state->has_key_tick[key_index] = true;

    if (key == 3) {
        led_brightness_t next_brightness =
            (led_brightness_t)((state->led_brightness + 1) % NUM_LED_BRIGHTNESS_LEVELS);
        return controller_set_led_brightness(state, next_brightness);
    }

    if (key > 3) {
        return controller_wake_led_brightness(state, now);
    }

    fan_speed_t next_speed = controller_next_fan_speed(state->fan_speed, key);
    ESP_RETURN_ON_ERROR(controller_set_fan_speed(state, next_speed), TAG,
                        "failed to transition fan speed");
    return controller_wake_led_brightness(state, now);
}

void app_main(void)
{
    // Fan control is initialized first so the decoder is disabled as early as
    // firmware can take control of its enable input.
    ESP_ERROR_CHECK(fan_init());
    ESP_LOGI(TAG, "Fan Selector Initialized.");

    ESP_ERROR_CHECK(leds_init());
    ESP_LOGI(TAG, "LED Driver Initialized.");

    controller_state_t state = {
        .fan_speed = FAN_SPEED_OFF,
        .led_brightness = LED_BRIGHTNESS_HIGH,
    };
    ESP_ERROR_CHECK(controller_set_led_brightness(&state, LED_BRIGHTNESS_HIGH));
    ESP_ERROR_CHECK(controller_set_fan_speed(&state, FAN_SPEED_OFF));

    ESP_ERROR_CHECK(touch_sens_init());
    ESP_LOGI(TAG, "Touch Sensor Initialized.");

    while (1) {
        TickType_t now = xTaskGetTickCount();
        uint8_t key = touch_sens_get_key_num();
        if (key != 0) {
            ESP_ERROR_CHECK(controller_handle_key(&state, key, now));
        }
        ESP_ERROR_CHECK(controller_service_led_wake(&state, now));

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
