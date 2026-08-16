#include "controller.h"
#include "diagnostics.h"
#include "fan_select.h"
#include "leds.h"
#include "network.h"
#include "ota_update.h"
#include "touch.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "MAIN_APP"
#define SETUP_SEQUENCE_TIMEOUT_MS 10000
#define OTA_BOOT_PROBATION_MS 30000

static void service_setup_sequence(uint8_t key, TickType_t now)
{
    static const uint8_t sequence[] = { 4, 5, 4, 5 };
    static size_t position;
    static TickType_t started;

    if (position != 0 && (now - started) > pdMS_TO_TICKS(SETUP_SEQUENCE_TIMEOUT_MS)) {
        position = 0;
    }
    if (key == sequence[position]) {
        if (position == 0) {
            started = now;
        }
        position++;
        if (position == sizeof(sequence)) {
            esp_err_t err = network_start_provisioning();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Could not open network setup: %s", esp_err_to_name(err));
            }
            position = 0;
        }
    } else {
        position = key == sequence[0] ? 1 : 0;
        if (position == 1) {
            started = now;
        }
    }
}

void app_main(void)
{
    diagnostics_init();
    bool ota_pending = ota_update_boot_is_pending();

    // Establish the hardware-off invariant before starting any peripheral or
    // network subsystem.
    ESP_ERROR_CHECK(fan_init());
    ESP_LOGI(TAG, "Fan Selector Initialized.");

    ESP_ERROR_CHECK(leds_init());
    ESP_LOGI(TAG, "LED Driver Initialized.");

    ESP_ERROR_CHECK(controller_init());
    ESP_LOGI(TAG, "Controller Initialized.");

    ESP_ERROR_CHECK(touch_sens_init());
    ESP_LOGI(TAG, "Touch Sensor Initialized.");

    esp_err_t network_err = network_init();
    if (network_err != ESP_OK) {
        // Network control is optional; never sacrifice local appliance
        // operation because the network stack could not start.
        controller_set_network_status(CONTROLLER_NETWORK_OFFLINE);
        ESP_LOGE(TAG, "Network initialization failed: %s", esp_err_to_name(network_err));
        if (ota_pending) {
            ESP_LOGE(TAG, "Pending OTA firmware failed network initialization; rolling back");
            diagnostics_mark_planned_restart(DIAGNOSTICS_RESTART_ROLLBACK);
            ota_update_rollback_running();
        }
    }

    TickType_t ota_probation_started = xTaskGetTickCount();
    if (ota_pending) {
        ESP_LOGW(TAG, "Pending OTA firmware entered 30-second probation");
    }

    while (1) {
        TickType_t now = xTaskGetTickCount();
        uint8_t key = touch_sens_get_key_num();
        if (key != 0 && !network_ota_is_busy()) {
            ESP_ERROR_CHECK(controller_handle_key(key, now));
            service_setup_sequence(key, now);
        }
        ESP_ERROR_CHECK(controller_service(now));
        network_service();
        if (ota_pending &&
            (now - ota_probation_started) >= pdMS_TO_TICKS(OTA_BOOT_PROBATION_MS)) {
            esp_err_t err = ota_update_confirm_running();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Could not confirm pending OTA firmware: %s", esp_err_to_name(err));
                diagnostics_mark_planned_restart(DIAGNOSTICS_RESTART_ROLLBACK);
                ota_update_rollback_running();
            } else {
                ESP_LOGI(TAG, "OTA firmware probation passed; image marked valid");
                ota_pending = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
