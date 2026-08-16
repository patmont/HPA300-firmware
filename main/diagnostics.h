#pragma once

#include <stdbool.h>

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

// Capture the reset reason before application initialization and consume any
// planned-reset marker retained in RTC memory. This does not access flash.
void diagnostics_init(void);

const diagnostics_boot_info_t *diagnostics_get_boot_info(void);

// Record why the application is about to call esp_restart(). The marker lives
// only in RTC no-init memory and is accepted only after a software reset.
void diagnostics_mark_planned_restart(diagnostics_restart_t reason);
