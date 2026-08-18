#include "ota_worker.h"

#include <string.h>

#include "fan_control.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "service_health.h"

// ESP-IDF's Xtensa port defines StackType_t as one byte and specifies task
// stack depths in bytes. Validation needs a measured 12 KiB allocation.
#define OTA_WORKER_STACK_BYTES (12U * 1024U)
#define OTA_WORKER_PRIORITY 4
#define OTA_BUFFER_SIZE 4096
#define OTA_BUFFER_COUNT 2
#define OTA_COMMAND_QUEUE_LENGTH 4
#define FAN_QUIESCE_TIMEOUT_MS 250
#define EVENT_COMMAND_DONE BIT0

typedef enum {
    COMMAND_BEGIN_AND_WRITE,
    COMMAND_WRITE,
    COMMAND_FINISH,
    COMMAND_ABORT,
    COMMAND_SELECT_PREVIOUS,
    COMMAND_CONFIRM_RUNNING,
    COMMAND_ROLLBACK_RUNNING,
} command_type_t;

typedef struct {
    command_type_t type;
    uint8_t buffer_index;
    size_t length;
    size_t image_size;
} command_t;

static StaticTask_t s_task_buffer;
static StackType_t s_task_stack[OTA_WORKER_STACK_BYTES];
static TaskHandle_t s_task;
static StaticQueue_t s_command_queue_buffer;
static uint8_t s_command_queue_storage[OTA_COMMAND_QUEUE_LENGTH * sizeof(command_t)];
static QueueHandle_t s_command_queue;
static StaticQueue_t s_free_queue_buffer;
static uint8_t s_free_queue_storage[OTA_BUFFER_COUNT * sizeof(uint8_t)];
static QueueHandle_t s_free_queue;
static StaticEventGroup_t s_event_buffer;
static EventGroupHandle_t s_events;
static StaticSemaphore_t s_submit_mutex_buffer;
static SemaphoreHandle_t s_submit_mutex;
static uint8_t s_buffers[OTA_BUFFER_COUNT][OTA_BUFFER_SIZE];
static portMUX_TYPE s_cache_lock = portMUX_INITIALIZER_UNLOCKED;
static ota_update_info_t s_info;
static ota_previous_validation_t s_validation = OTA_PREVIOUS_PENDING;
static volatile bool s_busy;
static esp_err_t s_result;
static char s_new_version[sizeof(((esp_app_desc_t *)0)->version)];

static int buffer_index(const uint8_t *buffer)
{
    for (unsigned i = 0; i < OTA_BUFFER_COUNT; ++i) {
        if (buffer == s_buffers[i]) {
            return (int)i;
        }
    }
    return -1;
}

static void refresh_validation_cache(void)
{
    ota_update_info_t info;
    esp_err_t err = ota_update_get_info(&info);
    portENTER_CRITICAL(&s_cache_lock);
    if (err == ESP_OK) {
        s_info = info;
        s_validation = info.previous_available ? OTA_PREVIOUS_VALID : OTA_PREVIOUS_INVALID;
    } else {
        memset(&s_info, 0, sizeof(s_info));
        s_validation = OTA_PREVIOUS_ERROR;
    }
    portEXIT_CRITICAL(&s_cache_lock);
}

static void release_command_buffer(const command_t *command)
{
    if (command->type == COMMAND_BEGIN_AND_WRITE || command->type == COMMAND_WRITE) {
        uint8_t index = command->buffer_index;
        xQueueSend(s_free_queue, &index, portMAX_DELAY);
    }
}

static void fail_maintenance(esp_err_t error)
{
    ota_update_abort();
    fan_control_end_maintenance();
    s_busy = false;
    service_health_set(SERVICE_OTA_WORKER, SERVICE_STATE_DEGRADED, error);
}

static esp_err_t execute_command(const command_t *command)
{
    switch (command->type) {
    case COMMAND_BEGIN_AND_WRITE: {
        esp_err_t err = fan_control_quiesce(pdMS_TO_TICKS(FAN_QUIESCE_TIMEOUT_MS));
        if (err != ESP_OK) {
            return err;
        }
        s_busy = true;
        err = ota_update_begin(s_buffers[command->buffer_index], command->length,
                               command->image_size, s_new_version, sizeof(s_new_version));
        if (err == ESP_OK) {
            err = ota_update_write(s_buffers[command->buffer_index], command->length);
        }
        if (err != ESP_OK) {
            fail_maintenance(err);
        }
        return err;
    }
    case COMMAND_WRITE: {
        esp_err_t err = ota_update_write(s_buffers[command->buffer_index], command->length);
        if (err != ESP_OK) {
            fail_maintenance(err);
        }
        return err;
    }
    case COMMAND_FINISH: {
        esp_err_t err = ota_update_finish();
        if (err != ESP_OK) {
            fail_maintenance(err);
        }
        return err;
    }
    case COMMAND_ABORT:
        fail_maintenance(ESP_FAIL);
        return ESP_OK;
    case COMMAND_SELECT_PREVIOUS: {
        esp_err_t err = fan_control_quiesce(pdMS_TO_TICKS(FAN_QUIESCE_TIMEOUT_MS));
        if (err != ESP_OK) {
            return err;
        }
        s_busy = true;
        err = ota_update_select_previous();
        if (err != ESP_OK) {
            fail_maintenance(err);
        }
        return err;
    }
    case COMMAND_CONFIRM_RUNNING: {
        esp_err_t err = fan_control_quiesce(pdMS_TO_TICKS(FAN_QUIESCE_TIMEOUT_MS));
        if (err != ESP_OK) {
            return err;
        }
        err = ota_update_confirm_running();
        fan_control_end_maintenance();
        return err;
    }
    case COMMAND_ROLLBACK_RUNNING:
        if (fan_control_quiesce(pdMS_TO_TICKS(FAN_QUIESCE_TIMEOUT_MS)) != ESP_OK) {
            return ESP_ERR_TIMEOUT;
        }
        return ota_update_rollback_running();
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static void ota_worker_task(void *argument)
{
    (void)argument;
    refresh_validation_cache();
    service_health_set(SERVICE_OTA_WORKER, SERVICE_STATE_READY, ESP_OK);
    while (true) {
        command_t command;
        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        s_result = execute_command(&command);
        release_command_buffer(&command);
        uint32_t free_stack =
            (uint32_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
        service_health_set_stack(SERVICE_OTA_WORKER, free_stack);
        if (free_stack < 2048U) {
            service_health_set(SERVICE_OTA_WORKER, SERVICE_STATE_DEGRADED, ESP_ERR_NO_MEM);
        } else if (s_result == ESP_OK) {
            service_health_set(SERVICE_OTA_WORKER, SERVICE_STATE_READY, ESP_OK);
        }
        xEventGroupSetBits(s_events, EVENT_COMMAND_DONE);
    }
}

esp_err_t ota_worker_init(void)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_command_queue = xQueueCreateStatic(OTA_COMMAND_QUEUE_LENGTH, sizeof(command_t),
                                         s_command_queue_storage, &s_command_queue_buffer);
    s_free_queue = xQueueCreateStatic(OTA_BUFFER_COUNT, sizeof(uint8_t),
                                      s_free_queue_storage, &s_free_queue_buffer);
    s_events = xEventGroupCreateStatic(&s_event_buffer);
    s_submit_mutex = xSemaphoreCreateMutexStatic(&s_submit_mutex_buffer);
    if (s_command_queue == NULL || s_free_queue == NULL || s_events == NULL ||
        s_submit_mutex == NULL) {
        service_health_set(SERVICE_OTA_WORKER, SERVICE_STATE_FAILED, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    for (uint8_t i = 0; i < OTA_BUFFER_COUNT; ++i) {
        xQueueSend(s_free_queue, &i, 0);
    }
    s_task = xTaskCreateStatic(ota_worker_task, "ota_worker", sizeof(s_task_stack),
                               NULL, OTA_WORKER_PRIORITY, s_task_stack, &s_task_buffer);
    if (s_task == NULL) {
        service_health_set(SERVICE_OTA_WORKER, SERVICE_STATE_FAILED, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t submit(command_t command)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_submit_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    xEventGroupClearBits(s_events, EVENT_COMMAND_DONE);
    if (xQueueSend(s_command_queue, &command, pdMS_TO_TICKS(1000)) != pdTRUE) {
        xSemaphoreGive(s_submit_mutex);
        return ESP_ERR_TIMEOUT;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, EVENT_COMMAND_DONE, pdTRUE, pdTRUE,
                                           portMAX_DELAY);
    esp_err_t result = (bits & EVENT_COMMAND_DONE) ? s_result : ESP_ERR_TIMEOUT;
    xSemaphoreGive(s_submit_mutex);
    return result;
}

bool ota_worker_is_busy(void)
{
    return s_busy;
}

uint8_t *ota_worker_acquire_buffer(TickType_t timeout, size_t *capacity)
{
    uint8_t index;
    if (s_free_queue == NULL || xQueueReceive(s_free_queue, &index, timeout) != pdTRUE) {
        return NULL;
    }
    if (capacity != NULL) {
        *capacity = OTA_BUFFER_SIZE;
    }
    return s_buffers[index];
}

void ota_worker_release_buffer(uint8_t *buffer)
{
    int index = buffer_index(buffer);
    if (index >= 0 && s_free_queue != NULL) {
        uint8_t value = (uint8_t)index;
        (void)xQueueSend(s_free_queue, &value, 0);
    }
}

esp_err_t ota_worker_begin_and_write(uint8_t *buffer, size_t header_length, size_t image_size)
{
    int index = buffer_index(buffer);
    if (index < 0 || header_length == 0 || header_length > OTA_BUFFER_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    return submit((command_t) { COMMAND_BEGIN_AND_WRITE, (uint8_t)index,
                                header_length, image_size });
}

esp_err_t ota_worker_write(uint8_t *buffer, size_t length)
{
    int index = buffer_index(buffer);
    if (index < 0 || length == 0 || length > OTA_BUFFER_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    return submit((command_t) { COMMAND_WRITE, (uint8_t)index, length, 0 });
}

esp_err_t ota_worker_finish(void) { return submit((command_t) { .type = COMMAND_FINISH }); }
void ota_worker_abort(void) { (void)submit((command_t) { .type = COMMAND_ABORT }); }

esp_err_t ota_worker_get_info(ota_update_info_t *info, ota_previous_validation_t *validation)
{
    if (info == NULL || validation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_cache_lock);
    *info = s_info;
    *validation = s_validation;
    portEXIT_CRITICAL(&s_cache_lock);
    return ESP_OK;
}

const char *ota_worker_validation_name(ota_previous_validation_t validation)
{
    switch (validation) {
    case OTA_PREVIOUS_PENDING: return "pending";
    case OTA_PREVIOUS_VALID: return "valid";
    case OTA_PREVIOUS_INVALID: return "invalid";
    case OTA_PREVIOUS_ERROR: return "error";
    default: return "error";
    }
}

esp_err_t ota_worker_select_previous(void)
{
    ota_previous_validation_t validation;
    ota_update_info_t info;
    ota_worker_get_info(&info, &validation);
    if (validation == OTA_PREVIOUS_PENDING) {
        return ESP_ERR_NOT_FINISHED;
    }
    if (validation != OTA_PREVIOUS_VALID) {
        return ESP_ERR_NOT_FOUND;
    }
    return submit((command_t) { .type = COMMAND_SELECT_PREVIOUS });
}

esp_err_t ota_worker_confirm_running(void)
{
    return submit((command_t) { .type = COMMAND_CONFIRM_RUNNING });
}

esp_err_t ota_worker_rollback_running(void)
{
    return submit((command_t) { .type = COMMAND_ROLLBACK_RUNNING });
}

const char *ota_worker_new_version(void) { return s_new_version; }
