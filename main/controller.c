#include "controller.h"

#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "leds.h"

#define TAG "CONTROLLER"

#define TOUCH_DEBOUNCE_MS 150
#define LED_WAKE_DURATION_MS 3000
#define NETWORK_LED LED_2
#define NETWORK_CONNECTING_TOGGLE_MS 500
#define NETWORK_PROVISIONING_TOGGLE_MS 250

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
    [LED_BRIGHTNESS_HIGH] = 255,
    [LED_BRIGHTNESS_MEDIUM] = 128,
    [LED_BRIGHTNESS_LOW] = 13,
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
    led_brightness_t led_brightness;
    fan_shutoff_mode_t fan_shutoff_mode;
    TickType_t fan_shutoff_started_tick;
    bool fan_shutoff_active;
    TickType_t last_key_tick[6];
    bool has_key_tick[6];
    TickType_t last_non_dimmer_touch_tick;
    bool brightness_wake_active;
    controller_source_t last_change_source;
    controller_network_status_t network_status;
    TickType_t network_led_last_toggle_tick;
    bool network_led_visible;
} controller_state_t;

static controller_state_t s_state;
static SemaphoreHandle_t s_lock;

static esp_err_t set_led_brightness_locked(led_brightness_t brightness);

static esp_err_t apply_network_led_locked(void)
{
    return leds_set_led_state(NETWORK_LED, s_state.network_led_visible);
}

static esp_err_t update_status_leds_locked(void)
{
    ESP_RETURN_ON_FALSE(s_state.fan_speed >= FAN_SPEED_OFF && s_state.fan_speed < NUM_FAN_SPEEDS,
                        ESP_ERR_INVALID_STATE, TAG, "invalid fan speed %d", (int)s_state.fan_speed);

    for (int i = 0; i < NUM_LEDS; i++) {
        ESP_RETURN_ON_ERROR(leds_set_led_state(i, false), TAG, "failed to turn off LED %d", i);
    }

    if (s_state.fan_speed != FAN_SPEED_OFF) {
        led_index_t led_index = fan_led_map[s_state.fan_speed];
        ESP_RETURN_ON_ERROR(leds_set_led_state((uint8_t)led_index, true), TAG,
                            "failed to turn on fan LED");

        led_index_t timer_led = s_fan_shutoff_led_map[s_state.fan_shutoff_mode];
        if (timer_led != LED_OFF) {
            ESP_RETURN_ON_ERROR(leds_set_led_state((uint8_t)timer_led, true), TAG,
                                "failed to turn on timer LED");
        }
    }
    return apply_network_led_locked();
}

static void cancel_fan_shutoff_locked(void)
{
    s_state.fan_shutoff_mode = FAN_SHUTOFF_ALWAYS_ON;
    s_state.fan_shutoff_active = false;
}

static esp_err_t set_fan_speed_locked(fan_speed_t speed, controller_source_t source,
                                      bool cancel_timer)
{
    ESP_RETURN_ON_FALSE(speed >= FAN_SPEED_OFF && speed < NUM_FAN_SPEEDS,
                        ESP_ERR_INVALID_ARG, TAG, "invalid fan speed %d", (int)speed);

    bool crosses_off = s_state.fan_speed != speed &&
                       (s_state.fan_speed == FAN_SPEED_OFF || speed == FAN_SPEED_OFF);

    ESP_RETURN_ON_ERROR(fan_select(speed), TAG, "failed to select fan speed %d", (int)speed);
    s_state.fan_speed = speed;
    s_state.last_change_source = source;
    if (speed == FAN_SPEED_OFF || cancel_timer) {
        cancel_fan_shutoff_locked();
    }

    esp_err_t err = update_status_leds_locked();
    if (err != ESP_OK) {
        fan_select(FAN_SPEED_OFF);
        s_state.fan_speed = FAN_SPEED_OFF;
        cancel_fan_shutoff_locked();
        update_status_leds_locked();
        return err;
    }

    if (crosses_off) {
        err = set_led_brightness_locked(LED_BRIGHTNESS_HIGH);
        if (err != ESP_OK) {
            fan_select(FAN_SPEED_OFF);
            s_state.fan_speed = FAN_SPEED_OFF;
            cancel_fan_shutoff_locked();
            update_status_leds_locked();
            return err;
        }
    }

    ESP_LOGI(TAG, "Fan speed = %d, source = %s", (int)speed,
             controller_source_name(source));
    return ESP_OK;
}

static esp_err_t set_fan_shutoff_mode_locked(fan_shutoff_mode_t mode, TickType_t now)
{
    ESP_RETURN_ON_FALSE(mode >= FAN_SHUTOFF_2H && mode < NUM_FAN_SHUTOFF_MODES,
                        ESP_ERR_INVALID_ARG, TAG, "invalid fan shutoff mode %d", (int)mode);
    ESP_RETURN_ON_FALSE(s_state.fan_speed != FAN_SPEED_OFF, ESP_ERR_INVALID_STATE, TAG,
                        "cannot set fan shutoff while fan is off");

    s_state.fan_shutoff_mode = mode;
    s_state.fan_shutoff_active = mode != FAN_SHUTOFF_ALWAYS_ON;
    if (s_state.fan_shutoff_active) {
        s_state.fan_shutoff_started_tick = now;
    }
    return update_status_leds_locked();
}

static esp_err_t apply_led_brightness_locked(led_brightness_t brightness)
{
    ESP_RETURN_ON_ERROR(leds_set_pwm_value(s_led_brightness_duty[brightness]), TAG,
                        "failed to set LED brightness");
    ESP_LOGI(TAG, "LED brightness set to %u%%",
             (unsigned)(s_led_brightness_duty[brightness] * 100U / 255U));
    return ESP_OK;
}

static esp_err_t set_led_brightness_locked(led_brightness_t brightness)
{
    ESP_RETURN_ON_FALSE(brightness >= LED_BRIGHTNESS_HIGH && brightness < NUM_LED_BRIGHTNESS_LEVELS,
                        ESP_ERR_INVALID_ARG, TAG, "invalid LED brightness %d", (int)brightness);
    s_state.led_brightness = brightness;
    s_state.brightness_wake_active = false;
    return apply_led_brightness_locked(brightness);
}

static esp_err_t wake_led_brightness_locked(TickType_t now)
{
    if (s_state.led_brightness == LED_BRIGHTNESS_HIGH) {
        return ESP_OK;
    }
    s_state.last_non_dimmer_touch_tick = now;
    s_state.brightness_wake_active = true;
    return apply_led_brightness_locked(LED_BRIGHTNESS_HIGH);
}

static esp_err_t service_led_wake_locked(TickType_t now)
{
    if (!s_state.brightness_wake_active ||
        (now - s_state.last_non_dimmer_touch_tick) < pdMS_TO_TICKS(LED_WAKE_DURATION_MS)) {
        return ESP_OK;
    }
    s_state.brightness_wake_active = false;
    return apply_led_brightness_locked(s_state.led_brightness);
}

static esp_err_t service_fan_shutoff_locked(TickType_t now)
{
    if (!s_state.fan_shutoff_active || s_state.fan_speed == FAN_SPEED_OFF) {
        return ESP_OK;
    }

    uint32_t duration_ms = s_fan_shutoff_duration_ms[s_state.fan_shutoff_mode];
    if ((now - s_state.fan_shutoff_started_tick) < pdMS_TO_TICKS(duration_ms)) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Fan shutoff timer expired");
    return set_fan_speed_locked(FAN_SPEED_OFF, CONTROLLER_SOURCE_TIMER, false);
}

static esp_err_t service_network_led_locked(TickType_t now)
{
    uint32_t toggle_ms;
    switch (s_state.network_status) {
        case CONTROLLER_NETWORK_CONNECTING:
            toggle_ms = NETWORK_CONNECTING_TOGGLE_MS;
            break;
        case CONTROLLER_NETWORK_PROVISIONING:
            toggle_ms = NETWORK_PROVISIONING_TOGGLE_MS;
            break;
        case CONTROLLER_NETWORK_OFFLINE:
        case CONTROLLER_NETWORK_CONNECTED:
        default:
            return ESP_OK;
    }

    if ((now - s_state.network_led_last_toggle_tick) < pdMS_TO_TICKS(toggle_ms)) {
        return ESP_OK;
    }
    s_state.network_led_last_toggle_tick = now;
    s_state.network_led_visible = !s_state.network_led_visible;
    return apply_network_led_locked();
}

static fan_speed_t next_fan_speed(fan_speed_t current_speed, uint8_t key)
{
    switch (key) {
        case 1:
            switch (current_speed) {
                case FAN_SPEED_OFF: return FAN_SPEED_1;
                case FAN_SPEED_1: return FAN_SPEED_2;
                case FAN_SPEED_2: return FAN_SPEED_3;
                default: return FAN_SPEED_OFF;
            }
        case 2:
            return current_speed == FAN_SPEED_4 ? FAN_SPEED_OFF : FAN_SPEED_4;
        default:
            return current_speed;
    }
}

esp_err_t controller_init(void)
{
    ESP_RETURN_ON_FALSE(s_lock == NULL, ESP_ERR_INVALID_STATE, TAG, "controller already initialized");
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "failed to create controller mutex");

    s_state = (controller_state_t) {
        .fan_speed = FAN_SPEED_OFF,
        .led_brightness = LED_BRIGHTNESS_HIGH,
        .fan_shutoff_mode = FAN_SHUTOFF_ALWAYS_ON,
        .last_change_source = CONTROLLER_SOURCE_BOOT,
        .network_status = CONTROLLER_NETWORK_OFFLINE,
    };

    ESP_RETURN_ON_ERROR(set_led_brightness_locked(LED_BRIGHTNESS_HIGH), TAG,
                        "failed to initialize LED brightness");
    return set_fan_speed_locked(FAN_SPEED_OFF, CONTROLLER_SOURCE_BOOT, false);
}

esp_err_t controller_handle_key(uint8_t key, TickType_t now)
{
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "controller not initialized");
    if (key < 1 || key > 6) {
        return ESP_OK;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t key_index = key - 1;
    if (s_state.has_key_tick[key_index] &&
        (now - s_state.last_key_tick[key_index]) < pdMS_TO_TICKS(TOUCH_DEBOUNCE_MS)) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    s_state.last_key_tick[key_index] = now;
    s_state.has_key_tick[key_index] = true;

    esp_err_t err = ESP_OK;
    if (key == 3) {
        led_brightness_t next = (led_brightness_t)((s_state.led_brightness + 1) % NUM_LED_BRIGHTNESS_LEVELS);
        err = set_led_brightness_locked(next);
    } else if (key == 6) {
        if (s_state.fan_speed != FAN_SPEED_OFF) {
            fan_shutoff_mode_t next = (fan_shutoff_mode_t)((s_state.fan_shutoff_mode + 1) % NUM_FAN_SHUTOFF_MODES);
            err = set_fan_shutoff_mode_locked(next, now);
        }
        if (err == ESP_OK) {
            err = wake_led_brightness_locked(now);
        }
    } else if (key > 3) {
        err = wake_led_brightness_locked(now);
    } else {
        err = set_fan_speed_locked(next_fan_speed(s_state.fan_speed, key),
                                   CONTROLLER_SOURCE_TOUCH, false);
        if (err == ESP_OK) {
            err = wake_led_brightness_locked(now);
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t controller_service(TickType_t now)
{
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "controller not initialized");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = service_led_wake_locked(now);
    if (err == ESP_OK) {
        err = service_fan_shutoff_locked(now);
    }
    if (err == ESP_OK) {
        err = service_network_led_locked(now);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t controller_set_remote_fan_speed(fan_speed_t speed)
{
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "controller not initialized");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // A remote command establishes HA as the authority, so a local timed-off
    // mode must not unexpectedly defeat the remote controller later.
    esp_err_t err = set_fan_speed_locked(speed, CONTROLLER_SOURCE_REST, true);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t controller_set_network_status(controller_network_status_t status)
{
    ESP_RETURN_ON_FALSE(status >= CONTROLLER_NETWORK_OFFLINE &&
                            status <= CONTROLLER_NETWORK_CONNECTED,
                        ESP_ERR_INVALID_ARG, TAG, "invalid network status %d", (int)status);
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "controller not initialized");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state.network_status == status) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    s_state.network_status = status;
    s_state.network_led_last_toggle_tick = xTaskGetTickCount();
    s_state.network_led_visible = status != CONTROLLER_NETWORK_OFFLINE;
    esp_err_t err = apply_network_led_locked();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t controller_get_snapshot(controller_snapshot_t *snapshot)
{
    ESP_RETURN_ON_FALSE(snapshot != NULL, ESP_ERR_INVALID_ARG, TAG, "snapshot is NULL");
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "controller not initialized");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snapshot->fan_speed = s_state.fan_speed;
    snapshot->led_brightness_percent =
        (uint8_t)(s_led_brightness_duty[s_state.led_brightness] * 100U / 255U);
    snapshot->shutoff_mode = (uint8_t)s_state.fan_shutoff_mode;
    snapshot->shutoff_active = s_state.fan_shutoff_active;
    snapshot->last_change_source = s_state.last_change_source;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

const char *controller_source_name(controller_source_t source)
{
    switch (source) {
        case CONTROLLER_SOURCE_BOOT: return "boot";
        case CONTROLLER_SOURCE_TOUCH: return "touch";
        case CONTROLLER_SOURCE_REST: return "rest";
        case CONTROLLER_SOURCE_TIMER: return "timer";
        default: return "unknown";
    }
}

const char *controller_shutoff_mode_name(uint8_t mode)
{
    switch ((fan_shutoff_mode_t)mode) {
        case FAN_SHUTOFF_2H: return "2h";
        case FAN_SHUTOFF_4H: return "4h";
        case FAN_SHUTOFF_8H: return "8h";
        case FAN_SHUTOFF_ALWAYS_ON: return "always_on";
        default: return "unknown";
    }
}
