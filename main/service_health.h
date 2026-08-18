#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    SERVICE_FAN = 0,
    SERVICE_UI,
    SERVICE_TOUCH,
    SERVICE_NETWORK,
    SERVICE_HTTP,
    SERVICE_DNS,
    SERVICE_OTA_WORKER,
    SERVICE_DIAGNOSTICS,
    SERVICE_COUNT,
} service_id_t;

typedef enum {
    SERVICE_STATE_STARTING = 0,
    SERVICE_STATE_READY,
    SERVICE_STATE_DEGRADED,
    SERVICE_STATE_RETRYING,
    SERVICE_STATE_FAILED,
} service_state_t;

typedef struct {
    service_state_t state;
    int32_t last_error;
    uint32_t restart_count;
    uint32_t minimum_free_stack_bytes;
} service_health_snapshot_t;

void service_health_init(void);
void service_health_set(service_id_t service, service_state_t state, esp_err_t error);
void service_health_note_restart(service_id_t service);
void service_health_set_stack(service_id_t service, uint32_t free_stack_bytes);
bool service_health_get(service_id_t service, service_health_snapshot_t *snapshot);
const char *service_health_name(service_id_t service);
const char *service_health_state_name(service_state_t state);
