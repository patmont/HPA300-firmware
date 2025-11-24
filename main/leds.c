
#include "leds.h"
#include "driver/ledc.h"
#include "hal/gpio_hal.h"
#include "rom/gpio.h"

#define LEDC_MODE           LEDC_LOW_SPEED_MODE
#define LEDC_TIMER_SEL      LEDC_TIMER_0
#define SHARED_LEDC_CH      LEDC_CHANNEL_0
#define PWM_RESOLUTION      LEDC_TIMER_8_BIT  // PWM resolution
#define PWM_FREQUENCY       5000              // Frequency in Hertz.  

// Global array to store the GPIO numbers
const gpio_num_t led_gpios[NUM_LEDS] = {
    GPIO_NUM_7, GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_10,
    GPIO_NUM_11, GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_15
};

// Global to store the internal signal ID for LEDC Channel 0 (used by the matrix)
static int ledc_signal_id;

void ledc_init() {
    // 1. Configure the single Timer
    ledc_timer_config_t ledc_timer = {
        .duty_resolution    = PWM_RESOLUTION,    // resolution of PWM duty
        .freq_hz            = PWM_FREQUENCY,     // frequency of PWM signal
        .speed_mode         = LEDC_MODE,         // timer mode
        .timer_num          = LEDC_TIMER_SEL,    // timer index
        .clk_cfg            = LEDC_AUTO_CLK,     // Auto select the source clock
    };
    // Set configuration of timer0
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

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
    ESP_ERROR_CHECK(ledc_channel_config(&single_channel_config));
    
    // --- 3. Determine the LEDC signal ID ---
    // This is hardware specific. LEDC low speed channel 0 signal index
    ledc_signal_id = LEDC_LS_SIG_OUT0_IDX + SHARED_LEDC_CH;

    // --- 4. Initialize all LEDs to the OFF state (disconnected from PWM) ---
    for (int i = 0; i < NUM_LEDS; i++) {
        leds_set_led_state(i, false); 
    }
}

void leds_set_pwm_value(uint32_t duty_value) {
    
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, SHARED_LEDC_CH, duty_value));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, SHARED_LEDC_CH));
    // All currently 'enabled' LEDs connected via the matrix instantly update their brightness.
}

void leds_set_led_state(uint8_t led_index, bool enable_pwm) {
    if (led_index >= NUM_LEDS) return;

    gpio_num_t pin = led_gpios[led_index];

    if (enable_pwm) {
        // Connect the specific LEDC signal to the GPIO pin using the matrix
        // Ensure pin is configured as output, then route the LEDC signal
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_matrix_out(pin, ledc_signal_id, false, false);
        
    } else {
        // Disconnect the PWM signal, revert to GPIO mode, and turn off completely
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_set_level(pin, 0); 
        gpio_matrix_out(pin, SIG_GPIO_OUT_IDX, false, false); // Explicitly disconnect peripheral
    }
}