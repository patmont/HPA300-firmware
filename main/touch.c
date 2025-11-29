#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/touch_sens.h"
#include "esp_log.h"
#include "touch_sens_config.h"

/* --- key → channel mapping --- */
#define NUM_KEYS 6
#define INIT_SCAN_TIMES 3

static const int s_channel_id[NUM_KEYS] = {
    1,  // key 1 = channel 1 = GPIO3
    2,  // key 2 = channel 2 = GPIO2
    3,  // key 3 = channel 3 = GPIO1
    5,  // key 4 = channel 5 = GPIO5
    6,  // key 5 = channel 6 = GPIO6
    4   // key 6 = channel 4 = GPIO4
};

// Active threshold to benchmark ratio. (i.e., touch will be activated when data >= benchmark * (1 + ratio))
static float s_thresh2bm_ratio[NUM_KEYS] = {
    [0 ... NUM_KEYS - 1] = 0.0001f,  // 1.5%
};

static uint8_t last_key = 0;
static const char *TAG = "touch_keys";

bool on_active(touch_sensor_handle_t sens_handle,
                    const touch_active_event_data_t *event,
                    void *user_ctx)
{
    // Map channel → key index
    for (int i = 0; i < NUM_KEYS; i++) {
        if (event->chan_id == s_channel_id[i]) {
            last_key = i + 1;   // key numbers start at 1
            break;
        }
    }    return false;
}

bool on_inactive(touch_sensor_handle_t sens_handle,
                    const touch_inactive_event_data_t *event,
                    void *user_ctx)
{
    return false;
}

static void do_initial_scanning(touch_sensor_handle_t sens_handle, touch_channel_handle_t chan_handle[])
{
    /* Enable the touch sensor to do the initial scanning, so that to initialize the channel data */
    ESP_ERROR_CHECK(touch_sensor_enable(sens_handle));

    /* Scan the enabled touch channels for several times, to make sure the initial channel data is stable */
    for (int i = 0; i < INIT_SCAN_TIMES; i++) {
        ESP_ERROR_CHECK(touch_sensor_trigger_oneshot_scanning(sens_handle, 5000));
    }

    /* Disable the touch channel to rollback the state */
    ESP_ERROR_CHECK(touch_sensor_disable(sens_handle));

    /* Read the initial channel benchmark and reconfig the channel active threshold accordingly */
    printf("Initial benchmark and new threshold are:\n");
    for (int i = 0; i < NUM_KEYS; i++) {
        /* Read the initial benchmark of the touch channel */
        uint32_t benchmark[TOUCH_SAMPLE_CFG_NUM] = {};
        ESP_ERROR_CHECK(touch_channel_read_data(chan_handle[i], TOUCH_CHAN_DATA_TYPE_BENCHMARK, benchmark));
        /* Calculate the proper active thresholds regarding the initial benchmark */
        printf("Touch [CH %d]", s_channel_id[i]);
        /* Generate the default channel configuration and then update the active threshold based on the real benchmark */
        touch_channel_config_t chan_cfg = TOUCH_CHAN_CFG_DEFAULT();
        for (int j = 0; j < TOUCH_SAMPLE_CFG_NUM; j++) {
            chan_cfg.active_thresh[j] = (uint32_t)(benchmark[j] * s_thresh2bm_ratio[i]);
        }
    }
}

void touch_sens_init(void)
{
    /* Handles of touch sensor */
    touch_sensor_handle_t sens_handle = NULL;
    touch_channel_handle_t chan_handle[NUM_KEYS];

    /* Step 1: Create a touch sensor controller handle */
    touch_sensor_sample_config_t sample_cfg[TOUCH_SAMPLE_CFG_NUM] = TOUCH_SAMPLE_CFG_DEFAULT();
    touch_sensor_config_t sens_cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(TOUCH_SAMPLE_CFG_NUM, sample_cfg);
    ESP_ERROR_CHECK(touch_sensor_new_controller(&sens_cfg, &sens_handle));

    /* Step 2: Create and enable the new touch channel handles */
    touch_channel_config_t chan_cfg = TOUCH_CHAN_CFG_DEFAULT();
    /* Allocate new touch channel on the touch controller */
    for (int i = 0; i < NUM_KEYS; i++) {
        ESP_ERROR_CHECK(touch_sensor_new_channel(sens_handle, s_channel_id[i], &chan_cfg, &chan_handle[i]));
    }

    /* Step 3: Configure the default filter for the touch sensor */
    touch_sensor_filter_config_t filter_cfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    ESP_ERROR_CHECK(touch_sensor_config_filter(sens_handle, &filter_cfg));

    /* Step 4: Do the initial scanning to initialize the channel data */
    do_initial_scanning(sens_handle, chan_handle);

    /* Step 5: Register the event callbacks for the touch sensor */
    touch_event_callbacks_t callbacks = {
        .on_active = on_active,
        .on_inactive = on_inactive,
    };
    ESP_ERROR_CHECK(touch_sensor_register_callbacks(sens_handle, &callbacks, NULL));

    /* Step 6: Enable the touch sensor */
    ESP_ERROR_CHECK(touch_sensor_enable(sens_handle));

    /* Step 7: Start continuous scanning */
    ESP_ERROR_CHECK(touch_sensor_start_continuous_scanning(sens_handle));
}

uint8_t touch_sens_get_key_num(void)
{
    uint8_t key = last_key;
    last_key = 0;   // reset after reading
    return key;
}
