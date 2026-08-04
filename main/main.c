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

// Temporary hardware-test durations. Keep the logical mode names and LED
// labels in hours; replace these with 2h, 4h, and 8h before production.
#define FAN_SHUTOFF_2H_DURATION_MS 2000
#define FAN_SHUTOFF_4H_DURATION_MS 4000
#define FAN_SHUTOFF_8H_DURATION_MS 8000

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

typedef enum {
    FAN_SHUTOFF_2H,
    FAN_SHUTOFF_4H,
    FAN_SHUTOFF_8H,
    FAN_SHUTOFF_ALWAYS_ON,
    NUM_FAN_SHUTOFF_MODES
} fan_shutoff_mode_t;

static const uint32_t s_fan_shutoff_duration_ms[NUM_FAN_SHUTOFF_MODES] = {
    [FAN_SHUTOFF_2H] = FAN_SHUTOFF_2H_DURATION_MS,
    [FAN_SHUTOFF_4H] = FAN_SHUTOFF_4H_DURATION_MS,
    [FAN_SHUTOFF_8H] = FAN_SHUTOFF_8H_DURATION_MS,
    [FAN_SHUTOFF_ALWAYS_ON] = 0,
};

static const led_index_t s_fan_shutoff_led_map[NUM_FAN_SHUTOFF_MODES] = {
    [FAN_SHUTOFF_2H] = LED_9,
    [FAN_SHUTOFF_4H] = LED_6,
    [FAN_SHUTOFF_8H] = LED_3,
    [FAN_SHUTOFF_ALWAYS_ON] = LED_OFF,
};

typedef struct {
    fan_speed_t fan_speed;
    led_brightness_t led_brightness; // User-selected, intended brightness
    fan_shutoff_mode_t fan_shutoff_mode;
    TickType_t fan_shutoff_started_tick;
    bool fan_shutoff_active;
    TickType_t last_key_tick[6];
    bool has_key_tick[6];
    TickType_t last_non_dimmer_touch_tick;
    bool brightness_wake_active;
} controller_state_t;

static esp_err_t controller_set_led_brightness(controller_state_t *state, led_brightness_t brightness);

static esp_err_t update_status_leds(const controller_state_t *state)
{
    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_INVALID_ARG, TAG, "controller state is NULL");
    ESP_RETURN_ON_FALSE(state->fan_speed >= FAN_SPEED_OFF && state->fan_speed < NUM_FAN_SPEEDS,
                        ESP_ERR_INVALID_ARG, TAG, "invalid fan speed %d", (int)state->fan_speed);
    ESP_RETURN_ON_FALSE(state->fan_shutoff_mode >= FAN_SHUTOFF_2H &&
                            state->fan_shutoff_mode < NUM_FAN_SHUTOFF_MODES,
                        ESP_ERR_INVALID_ARG, TAG, "invalid fan shutoff mode %d",
                        (int)state->fan_shutoff_mode);

    // Turn everything off first
    for (int i = 0; i < NUM_LEDS; i++) {
        ESP_RETURN_ON_ERROR(leds_set_led_state(i, false), TAG, "failed to turn off LED %d", i);
    }

    if (state->fan_speed != FAN_SPEED_OFF) {
        led_index_t led_index = fan_led_map[state->fan_speed];
        ESP_RETURN_ON_ERROR(leds_set_led_state((uint8_t)led_index, true), TAG,
                            "failed to turn on fan LED");

        led_index_t timer_led = s_fan_shutoff_led_map[state->fan_shutoff_mode];
        if (timer_led != LED_OFF) {
            ESP_RETURN_ON_ERROR(leds_set_led_state((uint8_t)timer_led, true), TAG,
                                "failed to turn on timer LED");
        }
        ESP_LOGI(TAG, "Fan speed = %d, timer mode = %d", (int)state->fan_speed,
                 (int)state->fan_shutoff_mode);
    }

    return ESP_OK;
}

static void controller_cancel_fan_shutoff(controller_state_t *state)
{
    state->fan_shutoff_mode = FAN_SHUTOFF_ALWAYS_ON;
    state->fan_shutoff_active = false;
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
    if (speed == FAN_SPEED_OFF) {
        controller_cancel_fan_shutoff(state);
    }

    esp_err_t err = update_status_leds(state);
    if (err != ESP_OK) {
        // A display failure must not leave an unrepresented active fan state.
        fan_select(FAN_SPEED_OFF);
        state->fan_speed = FAN_SPEED_OFF;
        controller_cancel_fan_shutoff(state);
        update_status_leds(state);
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
            controller_cancel_fan_shutoff(state);
            update_status_leds(state);
            return err;
        }
    }

    return ESP_OK;
}

static esp_err_t controller_set_fan_shutoff_mode(controller_state_t *state,
                                                  fan_shutoff_mode_t mode, TickType_t now)
{
    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_INVALID_ARG, TAG, "controller state is NULL");
    ESP_RETURN_ON_FALSE(mode >= FAN_SHUTOFF_2H && mode < NUM_FAN_SHUTOFF_MODES,
                        ESP_ERR_INVALID_ARG, TAG, "invalid fan shutoff mode %d", (int)mode);
    ESP_RETURN_ON_FALSE(state->fan_speed != FAN_SPEED_OFF, ESP_ERR_INVALID_STATE, TAG,
                        "cannot set fan shutoff while fan is off");

    state->fan_shutoff_mode = mode;
    state->fan_shutoff_active = mode != FAN_SHUTOFF_ALWAYS_ON;
    if (state->fan_shutoff_active) {
        state->fan_shutoff_started_tick = now;
    }

    return update_status_leds(state);
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

static esp_err_t controller_service_fan_shutoff(controller_state_t *state, TickType_t now)
{
    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_INVALID_ARG, TAG, "controller state is NULL");

    if (!state->fan_shutoff_active || state->fan_speed == FAN_SPEED_OFF) {
        return ESP_OK;
    }

    uint32_t duration_ms = s_fan_shutoff_duration_ms[state->fan_shutoff_mode];
    ESP_RETURN_ON_FALSE(duration_ms != 0, ESP_ERR_INVALID_STATE, TAG,
                        "active fan shutoff has no duration");
    if ((now - state->fan_shutoff_started_tick) < pdMS_TO_TICKS(duration_ms)) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Fan shutoff timer expired");
    return controller_set_fan_speed(state, FAN_SPEED_OFF);
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

static fan_shutoff_mode_t controller_next_fan_shutoff_mode(fan_shutoff_mode_t current_mode)
{
    return (fan_shutoff_mode_t)((current_mode + 1) % NUM_FAN_SHUTOFF_MODES);
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

    if (key == 6) {
        if (state->fan_speed == FAN_SPEED_OFF) {
            return controller_wake_led_brightness(state, now);
        }

        fan_shutoff_mode_t next_mode =
            controller_next_fan_shutoff_mode(state->fan_shutoff_mode);
        ESP_RETURN_ON_ERROR(controller_set_fan_shutoff_mode(state, next_mode, now), TAG,
                            "failed to set fan shutoff mode");
        return controller_wake_led_brightness(state, now);
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
        .fan_shutoff_mode = FAN_SHUTOFF_ALWAYS_ON,
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
        ESP_ERROR_CHECK(controller_service_fan_shutoff(&state, now));

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
