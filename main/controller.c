#include "controller.h"

#include "board.h"
#include "esp_check.h"
#include "freertos/semphr.h"
#include "leds.h"

#define TAG "CONTROLLER"
#define TOUCH_DEBOUNCE_MS 150
#define LED_WAKE_DURATION_MS 3000
#define CONTROLLER_LOCK_TIMEOUT_MS 20
#define NETWORK_LED LED_2
#define NETWORK_CONNECTING_TOGGLE_MS 500
#define NETWORK_PROVISIONING_TOGGLE_MS 250
#define NETWORK_UPDATING_TOGGLE_MS 100

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

static const led_index_t s_fan_shutoff_led_map[NUM_FAN_SHUTOFF_MODES] = {
    [FAN_SHUTOFF_2H] = LED_9,
    [FAN_SHUTOFF_4H] = LED_6,
    [FAN_SHUTOFF_8H] = LED_3,
    [FAN_SHUTOFF_ALWAYS_ON] = LED_OFF,
};

typedef struct {
    led_brightness_t led_brightness;
    TickType_t last_key_tick[6];
    bool has_key_tick[6];
    TickType_t last_non_dimmer_touch_tick;
    bool brightness_wake_active;
    controller_network_status_t network_status;
    TickType_t network_led_last_toggle_tick;
    bool network_led_visible;
    fan_speed_t rendered_speed;
    fan_shutoff_mode_t rendered_timer;
    bool rendered_fault;
} controller_ui_state_t;

static controller_ui_state_t s_ui;
static StaticSemaphore_t s_lock_buffer;
static SemaphoreHandle_t s_lock;
static bool s_led_available;

static bool take_lock(void)
{
    return s_lock != NULL &&
           xSemaphoreTake(s_lock, pdMS_TO_TICKS(CONTROLLER_LOCK_TIMEOUT_MS)) == pdTRUE;
}

static esp_err_t apply_network_led_locked(void)
{
    if (!s_led_available) {
        return ESP_OK;
    }
    return leds_set_led_state(NETWORK_LED, s_ui.network_led_visible);
}

static esp_err_t render_status_leds_locked(const fan_control_snapshot_t *fan)
{
    ESP_RETURN_ON_FALSE(fan->applied_speed < NUM_FAN_SPEEDS, ESP_ERR_INVALID_STATE,
                        TAG, "invalid fan speed %d", fan->applied_speed);
    if (!s_led_available) {
        s_ui.rendered_speed = fan->applied_speed;
        s_ui.rendered_timer = fan->shutoff_mode;
        s_ui.rendered_fault = fan->fault_latched;
        return ESP_OK;
    }
    for (int i = 0; i < NUM_LEDS; ++i) {
        ESP_RETURN_ON_ERROR(leds_set_led_state(i, false), TAG, "failed to turn off LED %d", i);
    }
    if (fan->applied_speed != FAN_SPEED_OFF && !fan->fault_latched) {
        ESP_RETURN_ON_ERROR(leds_set_led_state(fan_led_map[fan->applied_speed], true),
                            TAG, "failed to turn on fan LED");
        led_index_t timer_led = s_fan_shutoff_led_map[fan->shutoff_mode];
        if (timer_led != LED_OFF) {
            ESP_RETURN_ON_ERROR(leds_set_led_state(timer_led, true), TAG,
                                "failed to turn on timer LED");
        }
    }
    s_ui.rendered_speed = fan->applied_speed;
    s_ui.rendered_timer = fan->shutoff_mode;
    s_ui.rendered_fault = fan->fault_latched;
    return apply_network_led_locked();
}

static esp_err_t apply_led_brightness_locked(led_brightness_t brightness)
{
    if (!s_led_available) {
        return ESP_OK;
    }
    return leds_set_pwm_value(s_led_brightness_duty[brightness]);
}

static esp_err_t set_led_brightness_locked(led_brightness_t brightness)
{
    ESP_RETURN_ON_FALSE(brightness < NUM_LED_BRIGHTNESS_LEVELS, ESP_ERR_INVALID_ARG,
                        TAG, "invalid LED brightness %d", brightness);
    s_ui.led_brightness = brightness;
    s_ui.brightness_wake_active = false;
    return apply_led_brightness_locked(brightness);
}

static esp_err_t wake_led_brightness_locked(TickType_t now)
{
    if (s_ui.led_brightness == LED_BRIGHTNESS_HIGH) {
        return ESP_OK;
    }
    s_ui.last_non_dimmer_touch_tick = now;
    s_ui.brightness_wake_active = true;
    return apply_led_brightness_locked(LED_BRIGHTNESS_HIGH);
}

static esp_err_t service_led_wake_locked(TickType_t now)
{
    if (!s_ui.brightness_wake_active ||
        (now - s_ui.last_non_dimmer_touch_tick) < pdMS_TO_TICKS(LED_WAKE_DURATION_MS)) {
        return ESP_OK;
    }
    s_ui.brightness_wake_active = false;
    return apply_led_brightness_locked(s_ui.led_brightness);
}

static esp_err_t service_network_led_locked(TickType_t now)
{
    uint32_t toggle_ms;
    switch (s_ui.network_status) {
    case CONTROLLER_NETWORK_CONNECTING: toggle_ms = NETWORK_CONNECTING_TOGGLE_MS; break;
    case CONTROLLER_NETWORK_PROVISIONING: toggle_ms = NETWORK_PROVISIONING_TOGGLE_MS; break;
    case CONTROLLER_NETWORK_UPDATING: toggle_ms = NETWORK_UPDATING_TOGGLE_MS; break;
    default: return ESP_OK;
    }
    if ((now - s_ui.network_led_last_toggle_tick) < pdMS_TO_TICKS(toggle_ms)) {
        return ESP_OK;
    }
    s_ui.network_led_last_toggle_tick = now;
    s_ui.network_led_visible = !s_ui.network_led_visible;
    return apply_network_led_locked();
}

esp_err_t controller_init(void)
{
    ESP_RETURN_ON_FALSE(s_lock == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "controller already initialized");
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buffer);
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "failed to create UI mutex");
    s_ui = (controller_ui_state_t) {
        .led_brightness = LED_BRIGHTNESS_HIGH,
        .network_status = CONTROLLER_NETWORK_OFFLINE,
        .rendered_speed = NUM_FAN_SPEEDS,
        .rendered_timer = NUM_FAN_SHUTOFF_MODES,
    };
    (void)set_led_brightness_locked(LED_BRIGHTNESS_HIGH);
    fan_control_snapshot_t fan;
    ESP_RETURN_ON_ERROR(fan_control_get_snapshot(&fan), TAG, "fan actor is unavailable");
    (void)render_status_leds_locked(&fan);
    return ESP_OK;
}

void controller_set_led_available(bool available)
{
    s_led_available = available;
    if (available && take_lock()) {
        (void)apply_led_brightness_locked(s_ui.led_brightness);
        fan_control_snapshot_t fan;
        if (fan_control_get_snapshot(&fan) == ESP_OK) {
            (void)render_status_leds_locked(&fan);
        }
        xSemaphoreGive(s_lock);
    }
}

esp_err_t controller_handle_key(uint8_t key, TickType_t now)
{
    if (key < 1 || key > 6) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(take_lock(), ESP_ERR_TIMEOUT, TAG, "UI state busy");
    uint8_t index = key - 1;
    if (s_ui.has_key_tick[index] &&
        (now - s_ui.last_key_tick[index]) < pdMS_TO_TICKS(TOUCH_DEBOUNCE_MS)) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    s_ui.last_key_tick[index] = now;
    s_ui.has_key_tick[index] = true;

    esp_err_t err = ESP_OK;
    if (key == 1) {
        err = fan_control_submit_action(FAN_CONTROL_ACTION_CYCLE_NORMAL, NULL);
    } else if (key == 2) {
        err = fan_control_submit_action(FAN_CONTROL_ACTION_TOGGLE_TURBO, NULL);
    } else if (key == 3) {
        err = set_led_brightness_locked((led_brightness_t)((s_ui.led_brightness + 1) %
                                                           NUM_LED_BRIGHTNESS_LEVELS));
    } else if (key == 6) {
        err = fan_control_submit_action(FAN_CONTROL_ACTION_CYCLE_TIMER, NULL);
    }
    if (err == ESP_OK && key != 3) {
        err = wake_led_brightness_locked(now);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t controller_service(TickType_t now)
{
    ESP_RETURN_ON_FALSE(take_lock(), ESP_ERR_TIMEOUT, TAG, "UI state busy");
    esp_err_t err = service_led_wake_locked(now);
    if (err == ESP_OK) {
        err = service_network_led_locked(now);
    }
    fan_control_snapshot_t fan;
    if (err == ESP_OK && fan_control_get_snapshot(&fan) == ESP_OK &&
        (fan.applied_speed != s_ui.rendered_speed || fan.shutoff_mode != s_ui.rendered_timer ||
         fan.fault_latched != s_ui.rendered_fault)) {
        err = render_status_leds_locked(&fan);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t controller_set_remote_fan_speed(fan_speed_t speed, uint32_t *sequence)
{
    return fan_control_submit_speed(speed, FAN_CONTROL_SOURCE_REST, true, sequence);
}

esp_err_t controller_set_network_status(controller_network_status_t status)
{
    ESP_RETURN_ON_FALSE(status <= CONTROLLER_NETWORK_UPDATING, ESP_ERR_INVALID_ARG,
                        TAG, "invalid network status %d", status);
    ESP_RETURN_ON_FALSE(take_lock(), ESP_ERR_TIMEOUT, TAG, "UI state busy");
    if (s_ui.network_status == status) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    s_ui.network_status = status;
    s_ui.network_led_last_toggle_tick = xTaskGetTickCount();
    s_ui.network_led_visible = status != CONTROLLER_NETWORK_OFFLINE;
    esp_err_t err = apply_network_led_locked();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t controller_get_snapshot(controller_snapshot_t *snapshot)
{
    ESP_RETURN_ON_FALSE(snapshot != NULL, ESP_ERR_INVALID_ARG, TAG, "snapshot is NULL");
    fan_control_snapshot_t fan;
    ESP_RETURN_ON_ERROR(fan_control_get_snapshot(&fan), TAG, "failed to read fan actor");
    ESP_RETURN_ON_FALSE(take_lock(), ESP_ERR_TIMEOUT, TAG, "UI state busy");
    *snapshot = (controller_snapshot_t) {
        .fan_speed = fan.applied_speed,
        .desired_speed = fan.desired_speed,
        .led_brightness_percent =
            (uint8_t)(s_led_brightness_duty[s_ui.led_brightness] * 100U / 255U),
        .shutoff_mode = fan.shutoff_mode,
        .shutoff_active = fan.shutoff_active,
        .pending = fan.pending,
        .fault_latched = fan.fault_latched,
        .maintenance_active = fan.maintenance_active,
        .accepted_sequence = fan.accepted_sequence,
        .applied_sequence = fan.applied_sequence,
        .last_change_source = fan.last_change_source,
        .control_state = fan.state,
    };
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

const char *controller_source_name(controller_source_t source)
{
    return fan_control_source_name(source);
}

const char *controller_shutoff_mode_name(uint8_t mode)
{
    return fan_control_shutoff_mode_name((fan_shutoff_mode_t)mode);
}
