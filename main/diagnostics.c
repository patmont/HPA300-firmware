#include "diagnostics.h"

#include <stdint.h>

#include "esp_attr.h"
#include "esp_log.h"

#define TAG "DIAGNOSTICS"
#define RESTART_MARKER_MAGIC UINT32_C(0x48504133)

typedef struct {
    uint32_t magic;
    uint32_t magic_inverse;
    uint32_t reason;
    uint32_t reason_inverse;
} restart_marker_t;

static RTC_NOINIT_ATTR restart_marker_t s_restart_marker;
static diagnostics_boot_info_t s_boot_info;

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external_reset";
    case ESP_RST_SW: return "software_reset";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_EFUSE: return "efuse";
    case ESP_RST_PWR_GLITCH: return "power_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    case ESP_RST_UNKNOWN:
    default: return "unknown";
    }
}

static const char *planned_reason_name(diagnostics_restart_t reason)
{
    switch (reason) {
    case DIAGNOSTICS_RESTART_OTA: return "ota_install";
    case DIAGNOSTICS_RESTART_ROLLBACK: return "ota_rollback";
    case DIAGNOSTICS_RESTART_PROVISIONING: return "provisioning";
    case DIAGNOSTICS_RESTART_NONE:
    default: return "none";
    }
}

static bool marker_is_valid(diagnostics_restart_t *reason)
{
    if (s_restart_marker.magic != RESTART_MARKER_MAGIC ||
        s_restart_marker.magic_inverse != ~RESTART_MARKER_MAGIC ||
        s_restart_marker.reason != ~s_restart_marker.reason_inverse ||
        s_restart_marker.reason <= DIAGNOSTICS_RESTART_NONE ||
        s_restart_marker.reason > DIAGNOSTICS_RESTART_PROVISIONING) {
        return false;
    }
    *reason = (diagnostics_restart_t)s_restart_marker.reason;
    return true;
}

void diagnostics_init(void)
{
    diagnostics_restart_t planned_reason = DIAGNOSTICS_RESTART_NONE;
    s_boot_info.reset_reason = esp_reset_reason();
    s_boot_info.reset_reason_name = reset_reason_name(s_boot_info.reset_reason);
    s_boot_info.power_related = s_boot_info.reset_reason == ESP_RST_POWERON ||
                                s_boot_info.reset_reason == ESP_RST_BROWNOUT ||
                                s_boot_info.reset_reason == ESP_RST_PWR_GLITCH;
    s_boot_info.planned = s_boot_info.reset_reason == ESP_RST_SW &&
                          marker_is_valid(&planned_reason);
    s_boot_info.planned_reason = planned_reason_name(
        s_boot_info.planned ? planned_reason : DIAGNOSTICS_RESTART_NONE);

    // Consume the marker immediately. A later failure in this boot must not be
    // attributed to an old intentional restart.
    s_restart_marker = (restart_marker_t){ 0 };

    ESP_LOGW(TAG, "Last reset: %s (%d), planned=%s%s%s",
             s_boot_info.reset_reason_name, (int)s_boot_info.reset_reason,
             s_boot_info.planned ? "yes" : "no",
             s_boot_info.planned ? ", reason=" : "",
             s_boot_info.planned ? s_boot_info.planned_reason : "");
}

const diagnostics_boot_info_t *diagnostics_get_boot_info(void)
{
    return &s_boot_info;
}

void diagnostics_mark_planned_restart(diagnostics_restart_t reason)
{
    if (reason <= DIAGNOSTICS_RESTART_NONE ||
        reason > DIAGNOSTICS_RESTART_PROVISIONING) {
        return;
    }
    s_restart_marker.reason = (uint32_t)reason;
    s_restart_marker.reason_inverse = ~s_restart_marker.reason;
    s_restart_marker.magic_inverse = ~RESTART_MARKER_MAGIC;
    s_restart_marker.magic = RESTART_MARKER_MAGIC;
}
