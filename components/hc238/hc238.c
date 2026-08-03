#include "hc238.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "hc238";

// HC238 ensures only a single output is active high at any time. A0, A1, A2 select which output is high. E1 & E2 are active low; E3 is active high.
// Any of select or enable pins may be hardwired high or low by setting FIXED values in the config struct.

static hc238_config_t g_cfg;
static bool g_initialized;

static bool fixed_level_is_valid(int fixed_level)
{
    return fixed_level >= -1 && fixed_level <= 1;
}

static esp_err_t validate_pin(gpio_num_t gpio, int fixed_level, const char *name)
{
    if (!fixed_level_is_valid(fixed_level)) {
        ESP_LOGE(TAG, "%s fixed level must be -1, 0, or 1", name);
        return ESP_ERR_INVALID_ARG;
    }

    if (fixed_level < 0 && !GPIO_IS_VALID_OUTPUT_GPIO(gpio)) {
        ESP_LOGE(TAG, "%s requires a valid output GPIO", name);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t validate_config(const hc238_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "configuration is NULL");

    ESP_RETURN_ON_ERROR(validate_pin(cfg->A0_GPIO, cfg->A0_FIXED, "A0"), TAG, "invalid A0 configuration");
    ESP_RETURN_ON_ERROR(validate_pin(cfg->A1_GPIO, cfg->A1_FIXED, "A1"), TAG, "invalid A1 configuration");
    ESP_RETURN_ON_ERROR(validate_pin(cfg->A2_GPIO, cfg->A2_FIXED, "A2"), TAG, "invalid A2 configuration");
    ESP_RETURN_ON_ERROR(validate_pin(cfg->E1_GPIO, cfg->E1_FIXED, "E1"), TAG, "invalid E1 configuration");
    ESP_RETURN_ON_ERROR(validate_pin(cfg->E2_GPIO, cfg->E2_FIXED, "E2"), TAG, "invalid E2 configuration");
    ESP_RETURN_ON_ERROR(validate_pin(cfg->E3_GPIO, cfg->E3_FIXED, "E3"), TAG, "invalid E3 configuration");

    // All three fixed enable pins must be at their active levels for the
    // decoder to ever be enabled.
    ESP_RETURN_ON_FALSE(cfg->E1_FIXED < 0 || cfg->E1_FIXED == 0,
                        ESP_ERR_INVALID_ARG, TAG, "fixed E1 prevents enabling");
    ESP_RETURN_ON_FALSE(cfg->E2_FIXED < 0 || cfg->E2_FIXED == 0,
                        ESP_ERR_INVALID_ARG, TAG, "fixed E2 prevents enabling");
    ESP_RETURN_ON_FALSE(cfg->E3_FIXED < 0 || cfg->E3_FIXED == 1,
                        ESP_ERR_INVALID_ARG, TAG, "fixed E3 prevents enabling");

    // At least one enable input must be under MCU control so address changes
    // can always happen while all outputs are disabled.
    ESP_RETURN_ON_FALSE(cfg->E1_FIXED < 0 || cfg->E2_FIXED < 0 || cfg->E3_FIXED < 0,
                        ESP_ERR_INVALID_ARG, TAG, "no GPIO-controlled enable input");

    const gpio_num_t gpios[] = {
        cfg->A0_GPIO, cfg->A1_GPIO, cfg->A2_GPIO,
        cfg->E1_GPIO, cfg->E2_GPIO, cfg->E3_GPIO,
    };
    const int fixed_levels[] = {
        cfg->A0_FIXED, cfg->A1_FIXED, cfg->A2_FIXED,
        cfg->E1_FIXED, cfg->E2_FIXED, cfg->E3_FIXED,
    };
    for (size_t i = 0; i < sizeof(gpios) / sizeof(gpios[0]); i++) {
        if (fixed_levels[i] >= 0) {
            continue;
        }
        for (size_t j = i + 1; j < sizeof(gpios) / sizeof(gpios[0]); j++) {
            ESP_RETURN_ON_FALSE(fixed_levels[j] >= 0 || gpios[i] != gpios[j],
                                ESP_ERR_INVALID_ARG, TAG, "GPIO %d controls multiple HC238 inputs", gpios[i]);
        }
    }

    return ESP_OK;
}

static esp_err_t configure_output(gpio_num_t gpio, uint32_t initial_level)
{
    // Preload the output latch before switching the pad to output mode. This
    // avoids an active-level pulse during initialization.
    ESP_RETURN_ON_ERROR(gpio_set_level(gpio, initial_level), TAG, "failed to preload GPIO %d", gpio);
    ESP_RETURN_ON_ERROR(gpio_set_direction(gpio, GPIO_MODE_OUTPUT), TAG, "failed to configure GPIO %d", gpio);
    return ESP_OK;
}

static esp_err_t set_enable_levels(const hc238_config_t *cfg, bool enabled)
{
    // Disable E3 first, since this board uses E3 as the final active-high gate.
    if (!enabled && cfg->E3_FIXED < 0) {
        ESP_RETURN_ON_ERROR(gpio_set_level(cfg->E3_GPIO, 0), TAG, "failed to disable E3");
    }
    if (cfg->E1_FIXED < 0) {
        ESP_RETURN_ON_ERROR(gpio_set_level(cfg->E1_GPIO, enabled ? 0 : 1), TAG, "failed to set E1");
    }
    if (cfg->E2_FIXED < 0) {
        ESP_RETURN_ON_ERROR(gpio_set_level(cfg->E2_GPIO, enabled ? 0 : 1), TAG, "failed to set E2");
    }
    if (enabled && cfg->E3_FIXED < 0) {
        ESP_RETURN_ON_ERROR(gpio_set_level(cfg->E3_GPIO, 1), TAG, "failed to enable E3");
    }
    return ESP_OK;
}

// Initialize the GPIOs for HC238 control
esp_err_t hc238_init(const hc238_config_t *cfg)
{
    g_initialized = false;
    ESP_RETURN_ON_ERROR(validate_config(cfg), TAG, "invalid configuration");

    // Configure controlled enable pins at their inactive levels before any
    // address pins become outputs.
    if (cfg->E3_FIXED < 0) {
        ESP_RETURN_ON_ERROR(configure_output(cfg->E3_GPIO, 0), TAG, "failed to initialize E3");
    }
    if (cfg->E1_FIXED < 0) {
        ESP_RETURN_ON_ERROR(configure_output(cfg->E1_GPIO, 1), TAG, "failed to initialize E1");
    }
    if (cfg->E2_FIXED < 0) {
        ESP_RETURN_ON_ERROR(configure_output(cfg->E2_GPIO, 1), TAG, "failed to initialize E2");
    }

    if (cfg->A0_FIXED < 0) {
        ESP_RETURN_ON_ERROR(configure_output(cfg->A0_GPIO, 0), TAG, "failed to initialize A0");
    }
    if (cfg->A1_FIXED < 0) {
        ESP_RETURN_ON_ERROR(configure_output(cfg->A1_GPIO, 0), TAG, "failed to initialize A1");
    }
    if (cfg->A2_FIXED < 0) {
        ESP_RETURN_ON_ERROR(configure_output(cfg->A2_GPIO, 0), TAG, "failed to initialize A2");
    }

    g_cfg = *cfg;
    g_initialized = true;
    return ESP_OK;
}

esp_err_t hc238_enable(bool en)
{
    ESP_RETURN_ON_FALSE(g_initialized, ESP_ERR_INVALID_STATE, TAG, "driver is not initialized");
    return set_enable_levels(&g_cfg, en);
}

// Set the HC238 output based on the provided index (0-7)
// where index 3-bit binary value corresponds to A2, A1, A0 inputs
esp_err_t hc238_set_output(uint8_t index)
{
    ESP_RETURN_ON_FALSE(g_initialized, ESP_ERR_INVALID_STATE, TAG, "driver is not initialized");
    ESP_RETURN_ON_ERROR(hc238_enable(false), TAG, "failed to disable outputs");
    ESP_RETURN_ON_FALSE(index <= 7, ESP_ERR_INVALID_ARG, TAG, "output index must be 0-7");

    // Extract index value bits from for A0, A1, A2
    uint8_t bit0 = (index >> 0) & 1;
    uint8_t bit1 = (index >> 1) & 1;
    uint8_t bit2 = (index >> 2) & 1;

    // Verify the requested output is reachable before changing any address pin.
    ESP_RETURN_ON_FALSE(g_cfg.A0_FIXED < 0 || g_cfg.A0_FIXED == bit0,
                        ESP_ERR_INVALID_STATE, TAG, "A0 is fixed at the wrong level");
    ESP_RETURN_ON_FALSE(g_cfg.A1_FIXED < 0 || g_cfg.A1_FIXED == bit1,
                        ESP_ERR_INVALID_STATE, TAG, "A1 is fixed at the wrong level");
    ESP_RETURN_ON_FALSE(g_cfg.A2_FIXED < 0 || g_cfg.A2_FIXED == bit2,
                        ESP_ERR_INVALID_STATE, TAG, "A2 is fixed at the wrong level");

    if (g_cfg.A0_FIXED < 0) {
        ESP_RETURN_ON_ERROR(gpio_set_level(g_cfg.A0_GPIO, bit0), TAG, "failed to set A0");
    }
    if (g_cfg.A1_FIXED < 0) {
        ESP_RETURN_ON_ERROR(gpio_set_level(g_cfg.A1_GPIO, bit1), TAG, "failed to set A1");
    }
    if (g_cfg.A2_FIXED < 0) {
        ESP_RETURN_ON_ERROR(gpio_set_level(g_cfg.A2_GPIO, bit2), TAG, "failed to set A2");
    }

    return ESP_OK;
}
