#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_system.h"

typedef enum {
    DIAGNOSTICS_RESTART_NONE = 0,
    DIAGNOSTICS_RESTART_OTA,
    DIAGNOSTICS_RESTART_ROLLBACK,
    DIAGNOSTICS_RESTART_PROVISIONING,
} diagnostics_restart_t;

typedef struct {
    esp_reset_reason_t reset_reason;
    const char *reset_reason_name;
    bool power_related;
    bool planned;
    const char *planned_reason;
} diagnostics_boot_info_t;

typedef enum {
    DIAGNOSTICS_EVENT_BOOT = 1,
    DIAGNOSTICS_EVENT_TRANSITION_BEGIN,
    DIAGNOSTICS_EVENT_TRANSITION_COMMIT,
    DIAGNOSTICS_EVENT_TRANSITION_FAILURE,
    DIAGNOSTICS_EVENT_FAULT_LATCHED,
    DIAGNOSTICS_EVENT_MAINTENANCE_BEGIN,
    DIAGNOSTICS_EVENT_MAINTENANCE_END,
} diagnostics_event_t;

typedef struct {
    uint32_t sequence;
    diagnostics_event_t event;
    uint32_t uptime_ms;
    uint32_t value;
    int32_t detail;
} diagnostics_breadcrumb_t;

typedef struct {
    uint32_t count;
    uint32_t bytes;
    uint32_t time_us;
} diagnostics_flash_counter_t;

typedef struct {
    diagnostics_flash_counter_t read;
    diagnostics_flash_counter_t write;
    diagnostics_flash_counter_t erase;
} diagnostics_flash_counters_t;

typedef struct {
    uint32_t free_heap_bytes;
    uint32_t minimum_free_heap_bytes;
    diagnostics_flash_counters_t flash;
    bool coredump_present;
    bool coredump_readable;
    uint32_t coredump_size_bytes;
    uint32_t coredump_capacity_bytes;
} diagnostics_runtime_t;

// Capture the reset reason before application initialization and consume any
// planned-reset marker retained in RTC memory. This does not access flash.
void diagnostics_init(void);
esp_err_t diagnostics_start(void);

const diagnostics_boot_info_t *diagnostics_get_boot_info(void);

// Record why the application is about to call esp_restart(). The marker lives
// only in RTC no-init memory and is accepted only after a software reset.
void diagnostics_mark_planned_restart(diagnostics_restart_t reason);

void diagnostics_record_event(diagnostics_event_t event, uint32_t value, int32_t detail);
bool diagnostics_get_latest_event(diagnostics_breadcrumb_t *event);
const char *diagnostics_event_name(diagnostics_event_t event);

bool diagnostics_fan_fault_is_latched(void);
void diagnostics_latch_fan_fault(int32_t error, uint32_t phase);
void diagnostics_get_fan_fault(int32_t *error, uint32_t *phase);

// SDK counters are maintained in RAM and add no flash operations.
void diagnostics_get_flash_counters(diagnostics_flash_counters_t *counters);
void diagnostics_get_runtime(diagnostics_runtime_t *runtime);
