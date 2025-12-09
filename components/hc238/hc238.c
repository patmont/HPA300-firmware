#include "hc238.h"
#include "esp_log.h"

static const char *TAG = "hc238";

// HC238 ensures only a single output is active high at any time. A0, A1, A2 select which output is high. EN1 & EN2 are enabled low, EN2 is enable high.
// Any of select or enable pins may be hardwired high or low by setting FIXED values in the config struct.

static hc238_config_t g_cfg;

// Initialize the GPIOs for HC238 control
esp_err_t hc238_init(const hc238_config_t *cfg)
{
    g_cfg = *cfg;

    if (cfg->A0_GPIO != GPIO_NUM_NC)
        gpio_set_direction(cfg->A0_GPIO, GPIO_MODE_OUTPUT);
    
    if (cfg->A1_GPIO != GPIO_NUM_NC)
        gpio_set_direction(cfg->A1_GPIO, GPIO_MODE_OUTPUT);
    
    if (cfg->A2_GPIO != GPIO_NUM_NC)
        gpio_set_direction(cfg->A2_GPIO, GPIO_MODE_OUTPUT);
    
    if (cfg->E1_GPIO != GPIO_NUM_NC)
        gpio_set_direction(cfg->E1_GPIO, GPIO_MODE_OUTPUT);
    
    if (cfg->E2_GPIO != GPIO_NUM_NC)
        gpio_set_direction(cfg->E2_GPIO, GPIO_MODE_OUTPUT);
    
    if (cfg->E3_GPIO != GPIO_NUM_NC)
        gpio_set_direction(cfg->E3_GPIO, GPIO_MODE_OUTPUT);
    
    return ESP_OK;
}

esp_err_t hc238_enable(bool en) {
    // E1 active LOW
    if (g_cfg.E1_FIXED < 0 && g_cfg.E1_GPIO != GPIO_NUM_NC)
        gpio_set_level(g_cfg.E1_GPIO, en ? 0 : 1);

    // E2 active LOW
    if (g_cfg.E2_FIXED < 0 && g_cfg.E2_GPIO != GPIO_NUM_NC)
        gpio_set_level(g_cfg.E2_GPIO, en ? 0 : 1);

    // E3 active HIGH
    if (g_cfg.E3_FIXED < 0 && g_cfg.E3_GPIO != GPIO_NUM_NC)
        gpio_set_level(g_cfg.E3_GPIO, en ? 1 : 0);

    return ESP_OK;
}

// Set the HC238 output based on the provided index (0-7)
// where index 3-bit binary value corresponds to A2, A1, A0 inputs
esp_err_t hc238_set_output(uint8_t index) {
    
    hc238_enable(false); // Disable outputs while changing selection
    
    if (index > 7) return ESP_ERR_INVALID_ARG;
    
    // Extract index value bits from for A0, A1, A2
    uint8_t bit0 = (index >> 0) & 1;
    uint8_t bit1 = (index >> 1) & 1;
    uint8_t bit2 = (index >> 2) & 1;
    
    // Since we allow fixed-pin configuration, verify the output is achievable with the requested index //

    // A0
    if (g_cfg.A0_FIXED >= 0) {
        if (g_cfg.A0_FIXED != bit0)
                return ESP_ERR_INVALID_STATE;
    } else if (g_cfg.A0_GPIO != GPIO_NUM_NC) {
        gpio_set_level(g_cfg.A0_GPIO, bit0);
    }

    // A1
    if (g_cfg.A1_FIXED >= 0) {
        if (g_cfg.A1_FIXED != bit1)
            return ESP_ERR_INVALID_STATE;
    } else if (g_cfg.A1_GPIO != GPIO_NUM_NC) {
        gpio_set_level(g_cfg.A1_GPIO, bit1);
    }

    // A2
    if (g_cfg.A2_FIXED >= 0) {
        if (g_cfg.A2_FIXED != bit2)
            return ESP_ERR_INVALID_STATE;
    } else if (g_cfg.A2_GPIO != GPIO_NUM_NC) {
        gpio_set_level(g_cfg.A2_GPIO, bit2);
    }

    /* Write pins */
    // A0
    if (g_cfg.A0_FIXED < 0)
        gpio_set_level(g_cfg.A0_GPIO, bit0);

    // A1
    if (g_cfg.A1_FIXED < 0)
        gpio_set_level(g_cfg.A1_GPIO, bit1);

    // A2
    if (g_cfg.A2_FIXED < 0)
        gpio_set_level(g_cfg.A2_GPIO, bit2);

    return ESP_OK;
}


