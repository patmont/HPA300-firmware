#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "ota_update.h"

typedef enum {
    OTA_PREVIOUS_PENDING = 0,
    OTA_PREVIOUS_VALID,
    OTA_PREVIOUS_INVALID,
    OTA_PREVIOUS_ERROR,
} ota_previous_validation_t;

esp_err_t ota_worker_init(void);
bool ota_worker_is_busy(void);
uint8_t *ota_worker_acquire_buffer(TickType_t timeout, size_t *capacity);
void ota_worker_release_buffer(uint8_t *buffer);
esp_err_t ota_worker_begin_and_write(uint8_t *buffer, size_t header_length,
                                     size_t image_size);
esp_err_t ota_worker_write(uint8_t *buffer, size_t length);
esp_err_t ota_worker_finish(void);
void ota_worker_abort(void);
esp_err_t ota_worker_get_info(ota_update_info_t *info,
                              ota_previous_validation_t *validation);
const char *ota_worker_validation_name(ota_previous_validation_t validation);
esp_err_t ota_worker_select_previous(void);
esp_err_t ota_worker_confirm_running(void);
esp_err_t ota_worker_rollback_running(void);
const char *ota_worker_new_version(void);
