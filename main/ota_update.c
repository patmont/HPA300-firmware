#include "ota_update.h"

#include <inttypes.h>
#include <string.h>

#include "esp_app_format.h"
#include "esp_check.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define TAG "OTA_UPDATE"
#define HPA300_PROJECT_NAME "HPA300-FIRMWARE"
#define OTA_HEADER_LENGTH \
    (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

static esp_ota_handle_t s_handle;
static const esp_partition_t *s_target;
static size_t s_expected_size;
static size_t s_written;
static volatile bool s_busy;

static void partition_label(const esp_partition_t *partition, char *label, size_t size)
{
    if (partition == NULL) {
        strlcpy(label, "none", size);
    } else if (partition->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
               partition->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
        snprintf(label, size, "ota_%u",
                 (unsigned)(partition->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN));
    } else {
        strlcpy(label, partition->label, size);
    }
}

static const esp_app_desc_t *header_app_description(const uint8_t *header)
{
    return (const esp_app_desc_t *)(header + sizeof(esp_image_header_t) +
                                    sizeof(esp_image_segment_header_t));
}

static void copy_app_field(char *destination, size_t destination_size,
                           const char *source, size_t source_size)
{
    if (destination_size == 0) {
        return;
    }
    size_t length = strnlen(source, source_size);
    if (length >= destination_size) {
        length = destination_size - 1;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static esp_err_t erase_inactive_partition(const esp_partition_t *partition)
{
    ESP_RETURN_ON_FALSE(partition != NULL && partition->erase_size != 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid OTA partition");
    ESP_RETURN_ON_FALSE(partition->size % partition->erase_size == 0,
                        ESP_ERR_INVALID_SIZE, TAG, "OTA partition is not erase aligned");

    // A bulk erase of the 1.875 MiB slot can exceed the five-second task watchdog
    // window while the HTTP server task is active. Erase one hardware sector at a
    // time and block for one scheduler tick so the idle task and fan actor run.
    for (size_t offset = 0; offset < partition->size; offset += partition->erase_size) {
        esp_err_t err = esp_partition_erase_range(partition, offset, partition->erase_size);
        if (err != ESP_OK) {
            return err;
        }
        vTaskDelay(1);
    }
    return ESP_OK;
}

esp_err_t ota_update_begin(const uint8_t *image_header, size_t header_length,
                           size_t image_size, char *version, size_t version_size)
{
    ESP_RETURN_ON_FALSE(image_header != NULL && version != NULL && version_size > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid OTA arguments");
    ESP_RETURN_ON_FALSE(!s_busy, ESP_ERR_INVALID_STATE, TAG, "OTA already active");
    ESP_RETURN_ON_FALSE(header_length >= OTA_HEADER_LENGTH, ESP_ERR_OTA_VALIDATE_FAILED, TAG,
                        "firmware header is incomplete");

    const esp_image_header_t *image = (const esp_image_header_t *)image_header;
    const esp_app_desc_t *app = header_app_description(image_header);
    ESP_RETURN_ON_FALSE(image->magic == ESP_IMAGE_HEADER_MAGIC, ESP_ERR_OTA_VALIDATE_FAILED,
                        TAG, "firmware image magic is invalid");
    ESP_RETURN_ON_FALSE(image->chip_id == CONFIG_IDF_FIRMWARE_CHIP_ID,
                        ESP_ERR_OTA_VALIDATE_FAILED, TAG,
                        "firmware targets chip %u instead of %u", image->chip_id,
                        CONFIG_IDF_FIRMWARE_CHIP_ID);
    ESP_RETURN_ON_FALSE(app->magic_word == ESP_APP_DESC_MAGIC_WORD,
                        ESP_ERR_OTA_VALIDATE_FAILED, TAG,
                        "firmware application descriptor is invalid");
    ESP_RETURN_ON_FALSE(strncmp(app->project_name, HPA300_PROJECT_NAME,
                                sizeof(app->project_name)) == 0,
                        ESP_ERR_OTA_VALIDATE_FAILED, TAG,
                        "firmware project is not " HPA300_PROJECT_NAME);

    s_target = esp_ota_get_next_update_partition(NULL);
    ESP_RETURN_ON_FALSE(s_target != NULL, ESP_ERR_NOT_FOUND, TAG,
                        "inactive OTA partition was not found");
    ESP_RETURN_ON_FALSE(image_size > OTA_HEADER_LENGTH,
                        ESP_ERR_OTA_VALIDATE_FAILED, TAG,
                        "firmware image is too short");
    ESP_RETURN_ON_FALSE(image_size <= s_target->size, ESP_ERR_INVALID_SIZE, TAG,
                        "firmware size %u exceeds partition size %u",
                        (unsigned)image_size, (unsigned)s_target->size);

    copy_app_field(version, version_size, app->version, sizeof(app->version));
    ESP_LOGI(TAG, "Writing %s (%u bytes) to %s at 0x%" PRIx32,
             version, (unsigned)image_size, s_target->label, s_target->address);
    // Erase the entire inactive slot, not only the sectors covered by the
    // declared upload length. A Secure Boot v2 signature occupies a sector
    // after the unsigned image. If a shorter/unsigned upload has the same
    // payload as a previously signed image, leaving that sector untouched
    // would allow the stale signature to validate the new upload. Do it in
    // bounded operations rather than OTA_SIZE_UNKNOWN, which blocks while
    // erasing the complete partition and can trip the task watchdog.
    esp_err_t err = erase_inactive_partition(s_target);
    if (err != ESP_OK) {
        s_target = NULL;
        return err;
    }
    // esp_ota_begin() must still establish its normal rollback protections.
    // Request just one sector: the full slot was already erased above, and a
    // one-sector erase is bounded unlike an image-sized or unknown-size erase.
    err = esp_ota_begin(s_target, s_target->erase_size, &s_handle);
    if (err != ESP_OK) {
        s_target = NULL;
        return err;
    }
    s_expected_size = image_size;
    s_written = 0;
    s_busy = true;
    return ESP_OK;
}

esp_err_t ota_update_write(const void *data, size_t length)
{
    ESP_RETURN_ON_FALSE(s_busy && data != NULL && length > 0, ESP_ERR_INVALID_STATE,
                        TAG, "OTA is not active");
    ESP_RETURN_ON_FALSE(length <= s_expected_size - s_written, ESP_ERR_INVALID_SIZE,
                        TAG, "received more firmware data than declared");
    ESP_RETURN_ON_ERROR(esp_ota_write(s_handle, data, length), TAG,
                        "failed to write firmware");
    s_written += length;
    return ESP_OK;
}

esp_err_t ota_update_finish(void)
{
    ESP_RETURN_ON_FALSE(s_busy, ESP_ERR_INVALID_STATE, TAG, "OTA is not active");
    if (s_written != s_expected_size) {
        ESP_LOGE(TAG, "firmware was truncated: wrote %u of %u bytes",
                 (unsigned)s_written, (unsigned)s_expected_size);
        ota_update_abort();
        return ESP_ERR_INVALID_SIZE;
    }

    // esp_ota_end validates the ESP image and, with the project configuration,
    // its RSA signature. It also releases the OTA handle on failure.
    esp_err_t err = esp_ota_end(s_handle);
    s_handle = 0;
    s_busy = false;
    s_expected_size = 0;
    s_written = 0;
    if (err != ESP_OK) {
        s_target = NULL;
        return err;
    }

    err = esp_ota_set_boot_partition(s_target);
    s_target = NULL;
    return err;
}

void ota_update_abort(void)
{
    if (s_busy) {
        esp_ota_abort(s_handle);
    }
    s_handle = 0;
    s_target = NULL;
    s_expected_size = 0;
    s_written = 0;
    s_busy = false;
}

bool ota_update_is_busy(void)
{
    return s_busy;
}

esp_err_t ota_update_get_info(ota_update_info_t *info)
{
    ESP_RETURN_ON_FALSE(info != NULL, ESP_ERR_INVALID_ARG, TAG, "info is NULL");
    memset(info, 0, sizeof(*info));

    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_RETURN_ON_FALSE(running != NULL, ESP_ERR_NOT_FOUND, TAG,
                        "running partition was not found");
    partition_label(running, info->running_slot, sizeof(info->running_slot));
    esp_app_desc_t description;
    ESP_RETURN_ON_ERROR(esp_ota_get_partition_description(running, &description), TAG,
                        "failed to read running firmware description");
    copy_app_field(info->running_version, sizeof(info->running_version),
                   description.version, sizeof(description.version));

    info->previous_available = esp_ota_check_rollback_is_possible();
    if (info->previous_available) {
        const esp_partition_t *previous = esp_ota_get_next_update_partition(running);
        if (previous == NULL ||
            esp_ota_get_partition_description(previous, &description) != ESP_OK) {
            info->previous_available = false;
        } else {
            partition_label(previous, info->previous_slot, sizeof(info->previous_slot));
            copy_app_field(info->previous_version, sizeof(info->previous_version),
                           description.version, sizeof(description.version));
        }
    }
    return ESP_OK;
}

esp_err_t ota_update_select_previous(void)
{
    ESP_RETURN_ON_FALSE(!s_busy, ESP_ERR_INVALID_STATE, TAG, "OTA is active");
    ESP_RETURN_ON_FALSE(esp_ota_check_rollback_is_possible(), ESP_ERR_NOT_FOUND,
                        TAG, "no previous firmware is available");
    const esp_partition_t *previous =
        esp_ota_get_next_update_partition(esp_ota_get_running_partition());
    ESP_RETURN_ON_FALSE(previous != NULL, ESP_ERR_NOT_FOUND, TAG,
                        "previous partition was not found");
    return esp_ota_set_boot_partition(previous);
}

bool ota_update_boot_is_pending(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    return running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

esp_err_t ota_update_confirm_running(void)
{
    return esp_ota_mark_app_valid_cancel_rollback();
}

esp_err_t ota_update_rollback_running(void)
{
    return esp_ota_mark_app_invalid_rollback_and_reboot();
}
