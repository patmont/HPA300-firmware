#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "board.h"
#include "driver/touch_sens.h"
#include "esp_check.h"
#include "esp_log.h"
#include "touch_sens_config.h"

/* --- key → channel mapping --- */
#define NUM_KEYS 6
#define INIT_SCAN_TIMES 3
#define KEY_EVENT_QUEUE_LENGTH 8

// board.h is the single source of truth for electrode-to-key mapping.
static const int s_channel_id[NUM_KEYS] = {
    TOUCH_KEY1,
    TOUCH_KEY2,
    TOUCH_KEY3,
    TOUCH_KEY4,
    TOUCH_KEY5,
    TOUCH_KEY6,
};

// Active threshold to benchmark ratio. (i.e., touch will be activated when data >= benchmark * (1 + ratio))
static const float s_thresh2bm_ratio[NUM_KEYS] = {
    [0 ... NUM_KEYS - 1] = 0.015f,  // 1.5%
};

static const char *TAG = "touch_keys";
static touch_sensor_handle_t s_sensor_handle;
static touch_channel_handle_t s_channel_handle[NUM_KEYS];
static QueueHandle_t s_key_event_queue;

static void cleanup_touch(void)
{
    if (s_sensor_handle != NULL) {
        (void)touch_sensor_stop_continuous_scanning(s_sensor_handle);
        (void)touch_sensor_disable(s_sensor_handle);
    }
    if (s_key_event_queue != NULL) {
        vQueueDelete(s_key_event_queue);
        s_key_event_queue = NULL;
    }
    for (int i = NUM_KEYS - 1; i >= 0; --i) {
        if (s_channel_handle[i] != NULL) {
            (void)touch_sensor_del_channel(s_channel_handle[i]);
            s_channel_handle[i] = NULL;
        }
    }
    if (s_sensor_handle != NULL) {
        (void)touch_sensor_del_controller(s_sensor_handle);
        s_sensor_handle = NULL;
    }
}

static bool on_active(touch_sensor_handle_t sens_handle,
                      const touch_active_event_data_t *event,
                      void *user_ctx)
{
    (void)sens_handle;
    (void)user_ctx;

    uint8_t key = 0;
    for (int i = 0; i < NUM_KEYS; i++) {
        if (event->chan_id == s_channel_id[i]) {
            key = i + 1;
            break;
        }
    }

    BaseType_t high_priority_task_woken = pdFALSE;
    if (key != 0 && s_key_event_queue != NULL) {
        xQueueSendFromISR(s_key_event_queue, &key, &high_priority_task_woken);
    }
    return high_priority_task_woken == pdTRUE;
}

static esp_err_t do_initial_scanning(touch_sensor_handle_t sens_handle,
                                     touch_channel_handle_t chan_handle[])
{
    /* Enable the touch sensor to do the initial scanning, so that to initialize the channel data */
    ESP_RETURN_ON_ERROR(touch_sensor_enable(sens_handle), TAG, "failed to enable initial scan");

    /* Scan the enabled touch channels for several times, to make sure the initial channel data is stable */
    for (int i = 0; i < INIT_SCAN_TIMES; i++) {
        ESP_RETURN_ON_ERROR(touch_sensor_trigger_oneshot_scanning(sens_handle, 5000), TAG,
                            "initial scan %d failed", i);
    }

    /* Disable the touch channel to rollback the state */
    ESP_RETURN_ON_ERROR(touch_sensor_disable(sens_handle), TAG, "failed to disable after initial scan");

    /* Read the initial channel benchmark and reconfig the channel active threshold accordingly */
    printf("Initial benchmark and new threshold are:\n");
    for (int i = 0; i < NUM_KEYS; i++) {
        /* Read the initial benchmark of the touch channel */
        uint32_t benchmark[TOUCH_SAMPLE_CFG_NUM] = {};
        ESP_RETURN_ON_ERROR(touch_channel_read_data(chan_handle[i], TOUCH_CHAN_DATA_TYPE_BENCHMARK, benchmark),
                            TAG, "failed to read channel %d benchmark", (int)s_channel_id[i]);
        /* Calculate the proper active thresholds regarding the initial benchmark */
        printf("Touch [CH %d]", s_channel_id[i]);
        /* Generate the default channel configuration and then update the active threshold based on the real benchmark */
        touch_channel_config_t chan_cfg = TOUCH_CHAN_CFG_DEFAULT();
        for (int j = 0; j < TOUCH_SAMPLE_CFG_NUM; j++) {
            chan_cfg.active_thresh[j] = (uint32_t)(benchmark[j] * s_thresh2bm_ratio[i]);
            printf(" %d: %" PRIu32 ", %" PRIu32 "\t",
                   j, benchmark[j], chan_cfg.active_thresh[j]);
        }
        printf("\n");
        ESP_RETURN_ON_ERROR(touch_sensor_reconfig_channel(chan_handle[i], &chan_cfg), TAG,
                            "failed to configure channel %d threshold", (int)s_channel_id[i]);
    }

    return ESP_OK;
}

esp_err_t touch_sens_init(void)
{
    ESP_RETURN_ON_FALSE(s_sensor_handle == NULL, ESP_ERR_INVALID_STATE, TAG, "driver is already initialized");

    /* Step 1: Create a touch sensor controller handle */
    touch_sensor_sample_config_t sample_cfg[TOUCH_SAMPLE_CFG_NUM] = TOUCH_SAMPLE_CFG_DEFAULT();
    touch_sensor_config_t sens_cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(TOUCH_SAMPLE_CFG_NUM, sample_cfg);
    esp_err_t err = touch_sensor_new_controller(&sens_cfg, &s_sensor_handle);
    if (err != ESP_OK) {
        cleanup_touch();
        return err;
    }

    /* Step 2: Create and enable the new touch channel handles */
    touch_channel_config_t chan_cfg = TOUCH_CHAN_CFG_DEFAULT();
    /* Allocate new touch channel on the touch controller */
    for (int i = 0; i < NUM_KEYS; i++) {
        err = touch_sensor_new_channel(s_sensor_handle, s_channel_id[i], &chan_cfg,
                                       &s_channel_handle[i]);
        if (err != ESP_OK) {
            cleanup_touch();
            return err;
        }
    }

    /* Step 3: Configure the default filter for the touch sensor */
    touch_sensor_filter_config_t filter_cfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    err = touch_sensor_config_filter(s_sensor_handle, &filter_cfg);
    if (err != ESP_OK) {
        cleanup_touch();
        return err;
    }

    /* Step 4: Do the initial scanning to initialize the channel data */
    err = do_initial_scanning(s_sensor_handle, s_channel_handle);
    if (err != ESP_OK) {
        cleanup_touch();
        return err;
    }

    s_key_event_queue = xQueueCreate(KEY_EVENT_QUEUE_LENGTH, sizeof(uint8_t));
    if (s_key_event_queue == NULL) {
        cleanup_touch();
        return ESP_ERR_NO_MEM;
    }

    /* Step 5: Register the event callbacks for the touch sensor */
    touch_event_callbacks_t callbacks = {
        .on_active = on_active,
    };
    err = touch_sensor_register_callbacks(s_sensor_handle, &callbacks, NULL);
    if (err != ESP_OK) {
        cleanup_touch();
        return err;
    }

    /* Step 6: Enable the touch sensor */
    err = touch_sensor_enable(s_sensor_handle);
    if (err != ESP_OK) {
        cleanup_touch();
        return err;
    }

    /* Step 7: Start continuous scanning */
    err = touch_sensor_start_continuous_scanning(s_sensor_handle);
    if (err != ESP_OK) {
        cleanup_touch();
        return err;
    }
    return ESP_OK;
}

uint8_t touch_sens_get_key_num(void)
{
    uint8_t key = 0;
    if (s_key_event_queue != NULL) {
        xQueueReceive(s_key_event_queue, &key, 0);
    }
    return key;
}
