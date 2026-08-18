#include "diagnostics.h"

#include <stdint.h>

#include "esp_attr.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_spi_flash_counters.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "service_health.h"

#define TAG "DIAGNOSTICS"
#define RESTART_MARKER_MAGIC UINT32_C(0x48504133)
#define FAN_FAULT_MAGIC UINT32_C(0x46414e46)
#define BREADCRUMB_MAGIC UINT32_C(0x42524352)
#define BREADCRUMB_COUNT 16
#define DIAGNOSTICS_TASK_STACK_BYTES (6U * 1024U)
#define DIAGNOSTICS_PERIOD_MS 1000

typedef struct {
    uint32_t magic;
    uint32_t magic_inverse;
    uint32_t reason;
    uint32_t reason_inverse;
} restart_marker_t;

static RTC_NOINIT_ATTR restart_marker_t s_restart_marker;
static diagnostics_boot_info_t s_boot_info;

typedef struct {
    uint32_t magic;
    uint32_t magic_inverse;
    int32_t error;
    uint32_t phase;
    uint32_t checksum;
} fan_fault_marker_t;

typedef struct {
    uint32_t commit;
    uint32_t commit_inverse;
    diagnostics_breadcrumb_t value;
} breadcrumb_slot_t;

typedef struct {
    uint32_t magic;
    uint32_t magic_inverse;
    uint32_t next_sequence;
    breadcrumb_slot_t slots[BREADCRUMB_COUNT];
} breadcrumb_ring_t;

static RTC_NOINIT_ATTR fan_fault_marker_t s_fan_fault;
static RTC_NOINIT_ATTR breadcrumb_ring_t s_breadcrumbs;
static portMUX_TYPE s_breadcrumb_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_runtime_lock = portMUX_INITIALIZER_UNLOCKED;
static diagnostics_runtime_t s_runtime;
static StaticTask_t s_task_buffer;
static StackType_t s_task_stack[DIAGNOSTICS_TASK_STACK_BYTES];
static TaskHandle_t s_task;

static void sample_flash_counters(diagnostics_flash_counters_t *counters)
{
    const esp_flash_counters_t *sdk = esp_flash_get_counters();
    *counters = (diagnostics_flash_counters_t) {
        .read = { sdk->read.count, sdk->read.bytes, sdk->read.time },
        .write = { sdk->write.count, sdk->write.bytes, sdk->write.time },
        .erase = { sdk->erase.count, sdk->erase.bytes, sdk->erase.time },
    };
}

static void diagnostics_task(void *argument)
{
    (void)argument;
    diagnostics_runtime_t runtime = { 0 };
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (partition != NULL) {
        runtime.coredump_capacity_bytes = partition->size;
    }
    size_t dump_address = 0;
    size_t dump_size = 0;
    esp_err_t dump_err = esp_core_dump_image_get(&dump_address, &dump_size);
    runtime.coredump_present = dump_err == ESP_OK && dump_size != 0;
    runtime.coredump_size_bytes = runtime.coredump_present ? (uint32_t)dump_size : 0;
    runtime.coredump_readable = runtime.coredump_present &&
                                esp_core_dump_image_check() == ESP_OK;
    uint32_t logged_breadcrumb = 0;

    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        runtime.free_heap_bytes = esp_get_free_heap_size();
        runtime.minimum_free_heap_bytes = esp_get_minimum_free_heap_size();
        sample_flash_counters(&runtime.flash);
        service_health_set_stack(SERVICE_DIAGNOSTICS,
            (uint32_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
        service_health_set(SERVICE_DIAGNOSTICS, SERVICE_STATE_READY, ESP_OK);
        diagnostics_breadcrumb_t latest;
        if (diagnostics_get_latest_event(&latest) && latest.sequence != logged_breadcrumb) {
            ESP_LOGI(TAG, "event=%s sequence=%lu value=%lu detail=%ld uptime_ms=%lu",
                     diagnostics_event_name(latest.event), (unsigned long)latest.sequence,
                     (unsigned long)latest.value, (long)latest.detail,
                     (unsigned long)latest.uptime_ms);
            logged_breadcrumb = latest.sequence;
        }
        portENTER_CRITICAL(&s_runtime_lock);
        s_runtime = runtime;
        portEXIT_CRITICAL(&s_runtime_lock);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(DIAGNOSTICS_PERIOD_MS));
    }
}

static uint32_t fan_fault_checksum(int32_t error, uint32_t phase)
{
    return FAN_FAULT_MAGIC ^ (uint32_t)error ^ phase ^ UINT32_C(0xa55aa55a);
}

static bool fan_fault_is_valid(void)
{
    return s_fan_fault.magic == FAN_FAULT_MAGIC &&
           s_fan_fault.magic_inverse == ~FAN_FAULT_MAGIC &&
           s_fan_fault.checksum == fan_fault_checksum(s_fan_fault.error, s_fan_fault.phase);
}

static bool breadcrumb_ring_is_valid(void)
{
    return s_breadcrumbs.magic == BREADCRUMB_MAGIC &&
           s_breadcrumbs.magic_inverse == ~BREADCRUMB_MAGIC;
}

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

    // A fan driver fault intentionally survives reset-button, software,
    // watchdog, and panic resets. Only an actual cold boot clears it.
    if (s_boot_info.reset_reason == ESP_RST_POWERON) {
        s_fan_fault = (fan_fault_marker_t){ 0 };
    }
    if (!breadcrumb_ring_is_valid() || s_boot_info.reset_reason == ESP_RST_POWERON) {
        s_breadcrumbs = (breadcrumb_ring_t) {
            .magic = BREADCRUMB_MAGIC,
            .magic_inverse = ~BREADCRUMB_MAGIC,
            .next_sequence = 1,
        };
    }

    // Consume the marker immediately. A later failure in this boot must not be
    // attributed to an old intentional restart.
    s_restart_marker = (restart_marker_t){ 0 };

    ESP_LOGW(TAG, "Last reset: %s (%d), planned=%s%s%s",
             s_boot_info.reset_reason_name, (int)s_boot_info.reset_reason,
             s_boot_info.planned ? "yes" : "no",
             s_boot_info.planned ? ", reason=" : "",
             s_boot_info.planned ? s_boot_info.planned_reason : "");
    diagnostics_record_event(DIAGNOSTICS_EVENT_BOOT, (uint32_t)s_boot_info.reset_reason,
                             fan_fault_is_valid() ? s_fan_fault.error : 0);
}

esp_err_t diagnostics_start(void)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_task = xTaskCreateStatic(diagnostics_task, "diagnostics", sizeof(s_task_stack),
                               NULL, 2, s_task_stack, &s_task_buffer);
    if (s_task == NULL) {
        service_health_set(SERVICE_DIAGNOSTICS, SERVICE_STATE_FAILED, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
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

void diagnostics_record_event(diagnostics_event_t event, uint32_t value, int32_t detail)
{
    if (!breadcrumb_ring_is_valid()) {
        return;
    }
    uint32_t uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&s_breadcrumb_lock);
    uint32_t sequence = s_breadcrumbs.next_sequence++;
    if (sequence == 0) {
        sequence = 1;
        s_breadcrumbs.next_sequence = 2;
    }
    breadcrumb_slot_t *slot = &s_breadcrumbs.slots[(sequence - 1U) % BREADCRUMB_COUNT];
    slot->commit = 0;
    slot->commit_inverse = UINT32_MAX;
    slot->value = (diagnostics_breadcrumb_t) {
        .sequence = sequence,
        .event = event,
        .uptime_ms = uptime_ms,
        .value = value,
        .detail = detail,
    };
    slot->commit_inverse = ~sequence;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    slot->commit = sequence;
    portEXIT_CRITICAL(&s_breadcrumb_lock);
}

bool diagnostics_get_latest_event(diagnostics_breadcrumb_t *event)
{
    if (event == NULL) {
        return false;
    }
    portENTER_CRITICAL(&s_breadcrumb_lock);
    if (!breadcrumb_ring_is_valid() || s_breadcrumbs.next_sequence <= 1) {
        portEXIT_CRITICAL(&s_breadcrumb_lock);
        return false;
    }
    uint32_t sequence = s_breadcrumbs.next_sequence - 1U;
    const breadcrumb_slot_t *slot = &s_breadcrumbs.slots[(sequence - 1U) % BREADCRUMB_COUNT];
    if (slot->commit != sequence || slot->commit_inverse != ~sequence ||
        slot->value.sequence != sequence) {
        portEXIT_CRITICAL(&s_breadcrumb_lock);
        return false;
    }
    *event = slot->value;
    portEXIT_CRITICAL(&s_breadcrumb_lock);
    return true;
}

const char *diagnostics_event_name(diagnostics_event_t event)
{
    switch (event) {
    case DIAGNOSTICS_EVENT_BOOT: return "boot";
    case DIAGNOSTICS_EVENT_TRANSITION_BEGIN: return "transition_begin";
    case DIAGNOSTICS_EVENT_TRANSITION_COMMIT: return "transition_commit";
    case DIAGNOSTICS_EVENT_TRANSITION_FAILURE: return "transition_failure";
    case DIAGNOSTICS_EVENT_FAULT_LATCHED: return "fault_latched";
    case DIAGNOSTICS_EVENT_MAINTENANCE_BEGIN: return "maintenance_begin";
    case DIAGNOSTICS_EVENT_MAINTENANCE_END: return "maintenance_end";
    default: return "unknown";
    }
}

bool diagnostics_fan_fault_is_latched(void)
{
    return fan_fault_is_valid();
}

void diagnostics_latch_fan_fault(int32_t error, uint32_t phase)
{
    s_fan_fault.magic = 0;
    s_fan_fault.error = error;
    s_fan_fault.phase = phase;
    s_fan_fault.checksum = fan_fault_checksum(error, phase);
    s_fan_fault.magic_inverse = ~FAN_FAULT_MAGIC;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s_fan_fault.magic = FAN_FAULT_MAGIC;
    diagnostics_record_event(DIAGNOSTICS_EVENT_FAULT_LATCHED, phase, error);
}

void diagnostics_get_fan_fault(int32_t *error, uint32_t *phase)
{
    bool valid = fan_fault_is_valid();
    if (error != NULL) {
        *error = valid ? s_fan_fault.error : 0;
    }
    if (phase != NULL) {
        *phase = valid ? s_fan_fault.phase : 0;
    }
}

void diagnostics_get_flash_counters(diagnostics_flash_counters_t *counters)
{
    if (counters == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_runtime_lock);
    *counters = s_runtime.flash;
    portEXIT_CRITICAL(&s_runtime_lock);
}

void diagnostics_get_runtime(diagnostics_runtime_t *runtime)
{
    if (runtime == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_runtime_lock);
    *runtime = s_runtime;
    portEXIT_CRITICAL(&s_runtime_lock);
}
