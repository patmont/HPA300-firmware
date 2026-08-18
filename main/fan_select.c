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

static esp_err_t transition_enable(void *context, bool enabled)
{
    (void)context;
    return hc238_enable(enabled);
}

static esp_err_t transition_set_address(void *context, uint8_t address)
{
    (void)context;
    return hc238_set_address(address);
}

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

esp_err_t fan_transition(fan_speed_t speed, fan_transition_phase_t *phase)
{
    if (phase != NULL) {
        *phase = FAN_TRANSITION_PHASE_NONE;
    }
    if (speed < FAN_SPEED_OFF || speed >= NUM_FAN_SPEEDS) {
        ESP_LOGW(TAG, "Invalid fan speed %d", (int)speed);
        return ESP_ERR_INVALID_ARG;
    }

    static const uint8_t output_map[NUM_FAN_SPEEDS] = {
        [FAN_SPEED_OFF] = 0,
        [FAN_SPEED_1] = FAN_1,
        [FAN_SPEED_2] = FAN_2,
        [FAN_SPEED_3] = FAN_3,
        [FAN_SPEED_4] = FAN_4,
    };
    uint8_t out_idx = output_map[speed];
    fan_transition_backend_t backend = {
        .enable = transition_enable,
        .set_address = transition_set_address,
        .context = NULL,
    };
    return fan_transition_execute(speed == FAN_SPEED_OFF, out_idx, &backend, phase);
}
