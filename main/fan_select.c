#include "fan_select.h"
#include "board.h"
#include "hc238.h"
#include "esp_log.h"

static const char *TAG = "fan_select";

_Static_assert(BOARD_FAN_SPEED_COUNT == NUM_FAN_SPEEDS, "fan LED and speed maps must have equal size");
_Static_assert(FAN_1 <= 7 && FAN_2 <= 7 && FAN_3 <= 7 && FAN_4 <= 7,
               "HC238 output index must be 0-7");
_Static_assert(FAN_1 != FAN_2 && FAN_1 != FAN_3 && FAN_1 != FAN_4 &&
               FAN_2 != FAN_3 && FAN_2 != FAN_4 && FAN_3 != FAN_4,
               "fan speeds must use distinct HC238 outputs");
_Static_assert((FAN_1 & 1) == HC238_A0_FIXED && (FAN_2 & 1) == HC238_A0_FIXED &&
               (FAN_3 & 1) == HC238_A0_FIXED && (FAN_4 & 1) == HC238_A0_FIXED,
               "fan output mapping conflicts with hard-wired A0");

esp_err_t fan_init(void)
{
    hc238_config_t cfg = {
        .A0_GPIO = HC238_A0_GPIO,
        .A1_GPIO = HC238_A1_GPIO,
        .A2_GPIO = HC238_A2_GPIO,
        .E1_GPIO = HC238_E1_GPIO,
        .E2_GPIO = HC238_E2_GPIO,
        .E3_GPIO = HC238_E3_GPIO,
        .A0_FIXED = HC238_A0_FIXED,
        .A1_FIXED = HC238_A1_FIXED,
        .A2_FIXED = HC238_A2_FIXED,
        .E1_FIXED = HC238_E1_FIXED,
        .E2_FIXED = HC238_E2_FIXED,
        .E3_FIXED = HC238_E3_FIXED
    };

    esp_err_t err = hc238_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "HC238 initialized");
    return ESP_OK;
}

esp_err_t fan_enable(bool en)
{
    return hc238_enable(en);
}

esp_err_t fan_select(fan_speed_t speed)
{
    if (speed < FAN_SPEED_OFF || speed >= NUM_FAN_SPEEDS) {
        ESP_LOGW(TAG, "Invalid fan speed %d", (int)speed);
        return ESP_ERR_INVALID_ARG;
    }

    if (speed == FAN_SPEED_OFF) {
        ESP_LOGI(TAG, "Selecting FAN_OFF - disabling fan output");
        return hc238_enable(false);
    }

    static const uint8_t output_map[NUM_FAN_SPEEDS] = {
        [FAN_SPEED_OFF] = 0,
        [FAN_SPEED_1] = FAN_1,
        [FAN_SPEED_2] = FAN_2,
        [FAN_SPEED_3] = FAN_3,
        [FAN_SPEED_4] = FAN_4,
    };
    uint8_t out_idx = output_map[speed];

    ESP_LOGI(TAG, "Selecting fan %d -> HC238 output %d", (int)speed, out_idx);
    esp_err_t err = hc238_set_output(out_idx);
    if (err != ESP_OK) {
        return err;
    }

    err = hc238_enable(true);
    if (err != ESP_OK) {
        // Best effort: a failed transition must leave the decoder disabled.
        hc238_enable(false);
    }
    return err;
}
