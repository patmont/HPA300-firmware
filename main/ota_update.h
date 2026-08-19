#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_app_desc.h"
#include "esp_err.h"

typedef struct {
    char running_slot[8];
    char running_version[sizeof(((esp_app_desc_t *)0)->version)];
    bool previous_available;
    char previous_slot[8];
    char previous_version[sizeof(((esp_app_desc_t *)0)->version)];
} ota_update_info_t;

// The caller must retain the first bytes of the image until this returns. The
// bytes must include the image header, first segment header, and app descriptor.
esp_err_t ota_update_begin(const uint8_t *image_header, size_t header_length,
                           size_t image_size, char *version, size_t version_size);
esp_err_t ota_update_write(const void *data, size_t length);
esp_err_t ota_update_finish(void);
void ota_update_abort(void);

esp_err_t ota_update_get_info(ota_update_info_t *info);
esp_err_t ota_update_select_previous(void);

bool ota_update_boot_is_pending(void);
esp_err_t ota_update_confirm_running(void);
esp_err_t ota_update_rollback_running(void);
