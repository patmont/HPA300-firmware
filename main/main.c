#include "controller.h"
#include "diagnostics.h"
#include "fan_control.h"
#include "leds.h"
#include "network.h"
#include "ota_update.h"
#include "ota_worker.h"
#include "recovery_policy.h"
#include "service_health.h"
#include "touch.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "MAIN_APP"
#define SETUP_SEQUENCE_TIMEOUT_MS 10000
#define OTA_BOOT_PROBATION_MS 30000

#define retry_state_t recovery_policy_t
#define retry_failed(retry, now) \
    recovery_policy_mark_failed((retry), (uint32_t)(now), configTICK_RATE_HZ)
#define retry_due(retry, now) recovery_policy_is_due((retry), (uint32_t)(now))
#define retry_succeeded(retry) recovery_policy_mark_ready((retry))

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
    service_health_init();
    bool ota_pending = ota_update_boot_is_pending();

    // Establish the hardware-off invariant before starting any peripheral or
    // network subsystem.
    ESP_ERROR_CHECK(fan_control_init());
    ESP_LOGI(TAG, "Deterministic fan actor initialized OFF.");

    esp_err_t diagnostics_err = diagnostics_start();
    if (diagnostics_err != ESP_OK) {
        ESP_LOGE(TAG, "Diagnostics task unavailable: %s", esp_err_to_name(diagnostics_err));
    }

    esp_err_t ota_worker_err = ota_worker_init();
    if (ota_worker_err != ESP_OK) {
        ESP_LOGE(TAG, "OTA worker unavailable: %s", esp_err_to_name(ota_worker_err));
    }

    TickType_t startup_tick = xTaskGetTickCount();
    retry_state_t led_retry = { 0 };
    retry_state_t touch_retry = { 0 };
    retry_state_t network_retry = { 0 };

    esp_err_t led_err = leds_init();
    controller_set_led_available(led_err == ESP_OK);
    if (led_err == ESP_OK) {
        retry_succeeded(&led_retry);
        service_health_set(SERVICE_UI, SERVICE_STATE_STARTING, ESP_OK);
        ESP_LOGI(TAG, "LED driver initialized");
    } else {
        retry_failed(&led_retry, startup_tick);
        service_health_set(SERVICE_UI, SERVICE_STATE_DEGRADED, led_err);
        ESP_LOGE(TAG, "LED driver unavailable; local control remains active: %s",
                 esp_err_to_name(led_err));
    }

    esp_err_t controller_err = controller_init();
    service_health_set(SERVICE_UI,
                       controller_err == ESP_OK ? SERVICE_STATE_READY : SERVICE_STATE_DEGRADED,
                       controller_err);
    if (controller_err != ESP_OK) {
        ESP_LOGE(TAG, "UI controller unavailable; fan and REST remain active: %s",
                 esp_err_to_name(controller_err));
    }

    esp_err_t touch_err = touch_sens_init();
    if (touch_err == ESP_OK) {
        retry_succeeded(&touch_retry);
        service_health_set(SERVICE_TOUCH, SERVICE_STATE_READY, ESP_OK);
        ESP_LOGI(TAG, "Touch sensor initialized");
    } else {
        retry_failed(&touch_retry, startup_tick);
        service_health_set(SERVICE_TOUCH, SERVICE_STATE_DEGRADED, touch_err);
        ESP_LOGE(TAG, "Touch unavailable; fan and REST remain active: %s",
                 esp_err_to_name(touch_err));
    }

    esp_err_t network_err = network_init();
    if (network_err != ESP_OK) {
        // Network control is optional; never sacrifice local appliance
        // operation because the network stack could not start.
        controller_set_network_status(CONTROLLER_NETWORK_OFFLINE);
        retry_failed(&network_retry, startup_tick);
        service_health_set(SERVICE_NETWORK, SERVICE_STATE_DEGRADED, network_err);
        ESP_LOGE(TAG, "Network initialization failed: %s", esp_err_to_name(network_err));
        if (ota_pending) {
            ESP_LOGE(TAG, "Pending OTA firmware failed network initialization; rolling back");
            diagnostics_mark_planned_restart(DIAGNOSTICS_RESTART_ROLLBACK);
            ota_worker_rollback_running();
        }
    } else {
        retry_succeeded(&network_retry);
    }

    TickType_t ota_probation_started = xTaskGetTickCount();
    bool ota_network_seen = false;
    if (ota_pending) {
        ESP_LOGW(TAG, "Pending OTA firmware entered 30-second probation");
    }

    while (1) {
        TickType_t now = xTaskGetTickCount();
        static TickType_t last_manager_stack_sample;
        if ((now - last_manager_stack_sample) >= pdMS_TO_TICKS(1000)) {
            uint32_t free_stack =
                (uint32_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
            service_health_set_stack(SERVICE_UI, free_stack);
            service_health_set_stack(SERVICE_TOUCH, free_stack);
            service_health_set_stack(SERVICE_NETWORK, free_stack);
            last_manager_stack_sample = now;
        }
        if (retry_due(&led_retry, now)) {
            service_health_note_restart(SERVICE_UI);
            led_err = leds_init();
            if (led_err == ESP_OK) {
                retry_succeeded(&led_retry);
                controller_set_led_available(true);
                service_health_set(SERVICE_UI, SERVICE_STATE_READY, ESP_OK);
            } else {
                controller_set_led_available(false);
                retry_failed(&led_retry, now);
                service_health_set(SERVICE_UI, SERVICE_STATE_DEGRADED, led_err);
                ESP_LOGW(TAG, "LED retry failed: %s", esp_err_to_name(led_err));
            }
        }
        if (retry_due(&touch_retry, now)) {
            service_health_note_restart(SERVICE_TOUCH);
            touch_err = touch_sens_init();
            if (touch_err == ESP_OK) {
                retry_succeeded(&touch_retry);
                service_health_set(SERVICE_TOUCH, SERVICE_STATE_READY, ESP_OK);
            } else {
                retry_failed(&touch_retry, now);
                service_health_set(SERVICE_TOUCH, SERVICE_STATE_DEGRADED, touch_err);
                ESP_LOGW(TAG, "Touch retry failed: %s", esp_err_to_name(touch_err));
            }
        }
        if (retry_due(&network_retry, now)) {
            service_health_note_restart(SERVICE_NETWORK);
            network_err = network_init();
            if (network_err == ESP_OK) {
                retry_succeeded(&network_retry);
                service_health_set(SERVICE_NETWORK, SERVICE_STATE_READY, ESP_OK);
            } else {
                retry_failed(&network_retry, now);
                service_health_set(SERVICE_NETWORK, SERVICE_STATE_DEGRADED, network_err);
                ESP_LOGW(TAG, "Network service retry failed: %s",
                         esp_err_to_name(network_err));
            }
        }

        uint8_t key = touch_retry.ready ? touch_sens_get_key_num() : 0;
        if (key != 0 && !network_ota_is_busy()) {
            esp_err_t key_err = controller_handle_key(key, now);
            if (key_err != ESP_OK && key_err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "Local control request failed: %s", esp_err_to_name(key_err));
            }
            service_setup_sequence(key, now);
        }
        controller_err = controller_service(now);
        if (controller_err != ESP_OK && controller_err != ESP_ERR_TIMEOUT) {
            static TickType_t last_ui_error_log;
            service_health_set(SERVICE_UI, SERVICE_STATE_DEGRADED, controller_err);
            if ((now - last_ui_error_log) >= pdMS_TO_TICKS(1000)) {
                ESP_LOGW(TAG, "UI service degraded: %s", esp_err_to_name(controller_err));
                last_ui_error_log = now;
            }
        } else if (controller_err == ESP_OK && led_retry.ready) {
            service_health_set(SERVICE_UI, SERVICE_STATE_READY, ESP_OK);
        }
        network_service();
        if (ota_pending && network_is_connected()) {
            ota_network_seen = true;
        } else if (ota_pending && ota_network_seen) {
            ESP_LOGE(TAG, "Network failed during OTA probation; rolling back");
            diagnostics_mark_planned_restart(DIAGNOSTICS_RESTART_ROLLBACK);
            esp_err_t rollback_err = ota_worker_rollback_running();
            ESP_LOGE(TAG, "OTA rollback did not reboot: %s", esp_err_to_name(rollback_err));
            ota_network_seen = false;
            ota_probation_started = now;
        }
        if (ota_pending &&
            (now - ota_probation_started) >= pdMS_TO_TICKS(OTA_BOOT_PROBATION_MS)) {
            if (!network_is_connected()) {
                ESP_LOGE(TAG, "OTA probation ended without network; rolling back");
                diagnostics_mark_planned_restart(DIAGNOSTICS_RESTART_ROLLBACK);
                esp_err_t rollback_err = ota_worker_rollback_running();
                ESP_LOGE(TAG, "OTA rollback did not reboot: %s",
                         esp_err_to_name(rollback_err));
                ota_probation_started = now;
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            esp_err_t err = ota_worker_confirm_running();
            if (err != ESP_OK) {
                // Do not write OTA metadata unless OFF was acknowledged. A
                // healthy actor will retry after another probation interval;
                // a hung actor will be handled by its task watchdog.
                ESP_LOGE(TAG, "Could not quiesce fan for OTA confirmation: %s",
                         esp_err_to_name(err));
                if (err == ESP_ERR_TIMEOUT) {
                    ota_probation_started = now;
                } else {
                    ESP_LOGE(TAG, "Pending OTA firmware could not be confirmed; rolling back");
                    diagnostics_mark_planned_restart(DIAGNOSTICS_RESTART_ROLLBACK);
                    ota_worker_rollback_running();
                }
            } else {
                ESP_LOGI(TAG, "OTA firmware probation passed; image marked valid");
                ota_pending = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
