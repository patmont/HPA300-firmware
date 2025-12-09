#include "fan_select.h"
#include "esp_log.h"

static const char *TAG = "fan_select";

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

    ESP_ERROR_CHECK(hc238_init(&cfg));
    ESP_LOGI(TAG, "HC238 initialized");
    return ESP_OK;
}

esp_err_t fan_enable(bool en)
{
    ESP_ERROR_CHECK(hc238_enable(en));
    return ESP_OK;
}

esp_err_t fan_select(uint8_t fan_num)
{
    // Accept logical fan indices: 0 = OFF, 1..4 = fans
    if (fan_num > 4) {
        ESP_LOGW(TAG, "Invalid fan number %d; must be 0-4", fan_num);
        return ESP_ERR_INVALID_ARG;
    }

    if (fan_num == 0) {
        ESP_LOGI(TAG, "Selecting FAN_OFF — disabling fan output");
        // Turn off the HC238 driver (disable outputs)
        ESP_ERROR_CHECK(hc238_enable(false));
        return ESP_OK;
    }

    uint8_t out_idx = 0;
    switch (fan_num) {
        case 1: out_idx = FAN_1; break;
        case 2: out_idx = FAN_2; break;
        case 3: out_idx = FAN_3; break;
        case 4: out_idx = FAN_4; break;
        default:
            ESP_LOGW(TAG, "Unhandled fan number %d", fan_num);
            return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Selecting fan %d -> HC238 output %d", fan_num, out_idx);
    ESP_ERROR_CHECK(hc238_set_output(out_idx));
    ESP_ERROR_CHECK(hc238_enable(true));
    return ESP_OK;
}