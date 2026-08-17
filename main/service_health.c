#include "service_health.h"

#include <limits.h>

#include "freertos/FreeRTOS.h"

static service_health_snapshot_t s_services[SERVICE_COUNT];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void service_health_init(void)
{
    portENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < SERVICE_COUNT; ++i) {
        s_services[i] = (service_health_snapshot_t) {
            .state = SERVICE_STATE_STARTING,
            .minimum_free_stack_bytes = UINT32_MAX,
        };
    }
    portEXIT_CRITICAL(&s_lock);
}

void service_health_set(service_id_t service, service_state_t state, esp_err_t error)
{
    if (service < 0 || service >= SERVICE_COUNT) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_services[service].state = state;
    if (error != ESP_OK) {
        s_services[service].last_error = error;
    }
    portEXIT_CRITICAL(&s_lock);
}

void service_health_note_restart(service_id_t service)
{
    if (service < 0 || service >= SERVICE_COUNT) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_services[service].restart_count++;
    s_services[service].state = SERVICE_STATE_RETRYING;
    portEXIT_CRITICAL(&s_lock);
}

void service_health_set_stack(service_id_t service, uint32_t free_stack_bytes)
{
    if (service < 0 || service >= SERVICE_COUNT) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    if (free_stack_bytes < s_services[service].minimum_free_stack_bytes) {
        s_services[service].minimum_free_stack_bytes = free_stack_bytes;
    }
    portEXIT_CRITICAL(&s_lock);
}

bool service_health_get(service_id_t service, service_health_snapshot_t *snapshot)
{
    if (service < 0 || service >= SERVICE_COUNT || snapshot == NULL) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    *snapshot = s_services[service];
    portEXIT_CRITICAL(&s_lock);
    if (snapshot->minimum_free_stack_bytes == UINT32_MAX) {
        snapshot->minimum_free_stack_bytes = 0;
    }
    return true;
}

const char *service_health_name(service_id_t service)
{
    static const char *const names[SERVICE_COUNT] = {
        "fan", "ui", "touch", "network", "http", "dns", "ota_worker", "diagnostics",
    };
    return service >= 0 && service < SERVICE_COUNT ? names[service] : "unknown";
}

const char *service_health_state_name(service_state_t state)
{
    switch (state) {
    case SERVICE_STATE_STARTING: return "starting";
    case SERVICE_STATE_READY: return "ready";
    case SERVICE_STATE_DEGRADED: return "degraded";
    case SERVICE_STATE_RETRYING: return "retrying";
    case SERVICE_STATE_FAILED: return "failed";
    default: return "unknown";
    }
}
