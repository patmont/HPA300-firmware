
#include "board.h"
#include "leds.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "hal/gpio_hal.h"
#include "rom/gpio.h"

#define LEDC_MODE           LEDC_LOW_SPEED_MODE
#define LEDC_TIMER_SEL      LEDC_TIMER_0
#define SHARED_LEDC_CH      LEDC_CHANNEL_0
#define PWM_RESOLUTION      LEDC_TIMER_8_BIT  // PWM resolution
#define PWM_FREQUENCY       5000              // Frequency in Hertz.  
#define PWM_MAX_DUTY        ((1U << PWM_RESOLUTION) - 1U)

static const char *TAG = "leds";

// Global to store the internal signal ID for LEDC Channel 0 (used by the matrix)
static int ledc_signal_id;
static bool s_initialized;

esp_err_t leds_init(void)
{
    s_initialized = false;

    // 1. Configure the single Timer
    ledc_timer_config_t ledc_timer = {
        .duty_resolution    = PWM_RESOLUTION,    // resolution of PWM duty
        .freq_hz            = PWM_FREQUENCY,     // frequency of PWM signal
        .speed_mode         = LEDC_MODE,         // timer mode
        .timer_num          = LEDC_TIMER_SEL,    // timer index
        .clk_cfg            = LEDC_AUTO_CLK,     // Auto select the source clock
    };
    // Set configuration of timer0
    ESP_RETURN_ON_ERROR(ledc_timer_config(&ledc_timer), TAG, "failed to configure LEDC timer");

    // --- 2. Configure the single Channel ---
    // We configure the channel using an arbitrary first GPIO number; 
    // the matrix functions will override/expand this later.
    ledc_channel_config_t single_channel_config = {
        .channel                = SHARED_LEDC_CH,
        .duty                   = 0,
        .gpio_num               = led_gpios[0], // Only one pin specified here in the struct
        .speed_mode             = LEDC_MODE,
        .hpoint                 = 0,
        .timer_sel              = LEDC_TIMER_SEL,
        .flags.output_invert    = 0
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&single_channel_config), TAG, "failed to configure LEDC channel");
    
    // --- 3. Determine the LEDC signal ID ---
    // This is hardware specific. LEDC low speed channel 0 signal index
    ledc_signal_id = LEDC_LS_SIG_OUT0_IDX + SHARED_LEDC_CH;
    s_initialized = true;

    // --- 4. Initialize all LEDs to the OFF state (disconnected from PWM) ---
    for (int i = 0; i < NUM_LEDS; i++) {
        ESP_RETURN_ON_ERROR(leds_set_led_state(i, false), TAG, "failed to initialize LED %d", i);
    }

    return ESP_OK;
}

esp_err_t leds_set_pwm_value(uint32_t duty_value)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "driver is not initialized");
    ESP_RETURN_ON_FALSE(duty_value <= PWM_MAX_DUTY, ESP_ERR_INVALID_ARG, TAG,
                        "duty must be 0-%u", (unsigned)PWM_MAX_DUTY);
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_MODE, SHARED_LEDC_CH, duty_value), TAG, "failed to set duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_MODE, SHARED_LEDC_CH), TAG, "failed to update duty");
    // All currently 'enabled' LEDs connected via the matrix instantly update their brightness.
    return ESP_OK;
}

esp_err_t leds_set_led_state(uint8_t led_index, bool enable_pwm)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "driver is not initialized");
    ESP_RETURN_ON_FALSE(led_index < NUM_LEDS, ESP_ERR_INVALID_ARG, TAG, "invalid LED index %u", led_index);

    gpio_num_t pin = led_gpios[led_index];

    if (enable_pwm) {
        // Connect the specific LEDC signal to the GPIO pin using the matrix
        // Ensure pin is configured as output, then route the LEDC signal
        ESP_RETURN_ON_ERROR(gpio_set_direction(pin, GPIO_MODE_OUTPUT), TAG, "failed to configure LED %u", led_index);
        gpio_matrix_out(pin, ledc_signal_id, false, false);
        
    } else {
        // Disconnect the PWM signal, revert to GPIO mode, and turn off completely
        ESP_RETURN_ON_ERROR(gpio_set_direction(pin, GPIO_MODE_OUTPUT), TAG, "failed to configure LED %u", led_index);
        ESP_RETURN_ON_ERROR(gpio_set_level(pin, 0), TAG, "failed to turn off LED %u", led_index);
        gpio_matrix_out(pin, SIG_GPIO_OUT_IDX, false, false); // Explicitly disconnect peripheral
    }

    return ESP_OK;
}
