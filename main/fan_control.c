#include "fan_control.h"

#include <limits.h>
#include <string.h>

#include "diagnostics.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "fan_policy.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define TAG "FAN_CONTROL"
#define CONTROL_PERIOD_MS 10
#define CONTROL_BUDGET_US 1000
#define CONTROL_DEADLINE_US 20000
#define CONTROL_TASK_STACK_WORDS 3072
#define CONTROL_TASK_PRIORITY (configMAX_PRIORITIES - 2)
#define INIT_TIMEOUT_MS 1000
#define EVENT_INITIALIZED BIT0
#define EVENT_QUIESCED BIT1

// Temporary hardware-test durations retained from the existing controller.
#define FAN_SHUTOFF_2H_DURATION_MS 2000
#define FAN_SHUTOFF_4H_DURATION_MS 4000
#define FAN_SHUTOFF_8H_DURATION_MS 8000

typedef struct {
    fan_control_action_t action;
    fan_speed_t speed;
    fan_control_source_t source;
    bool cancel_timer;
    uint32_t sequence;
} fan_command_t;

typedef struct {
    uint32_t begin_sequence;
    uint32_t version;
    uint32_t size;
    uint32_t reset_reason;
    uint32_t uptime_ms;
    fan_control_snapshot_t control;
    diagnostics_flash_counters_t flash;
    uint32_t free_heap_bytes;
    uint32_t minimum_free_heap_bytes;
    uint32_t reserved[12];
    uint32_t end_sequence;
} fan_coredump_snapshot_t;

static QueueHandle_t s_mailbox;
static StaticQueue_t s_mailbox_buffer;
static uint8_t s_mailbox_storage[sizeof(fan_command_t)];
static EventGroupHandle_t s_events;
static StaticEventGroup_t s_event_buffer;
static StaticTask_t s_task_buffer;
static StackType_t s_task_stack[CONTROL_TASK_STACK_WORDS];
static TaskHandle_t s_task;
static esp_err_t s_init_result = ESP_ERR_INVALID_STATE;
static volatile uint32_t s_snapshot_sequence;
static fan_control_snapshot_t s_snapshot;
static volatile uint32_t s_command_sequence;
static volatile uint32_t s_coalesced_commands;
static volatile uint32_t s_rejected_commands;
static volatile bool s_maintenance_requested;
static portMUX_TYPE s_command_lock = portMUX_INITIALIZER_UNLOCKED;
static COREDUMP_DRAM_ATTR fan_coredump_snapshot_t s_coredump_snapshot;

static const uint32_t s_shutoff_duration_ms[NUM_FAN_SHUTOFF_MODES] = {
    [FAN_SHUTOFF_2H] = FAN_SHUTOFF_2H_DURATION_MS,
    [FAN_SHUTOFF_4H] = FAN_SHUTOFF_4H_DURATION_MS,
    [FAN_SHUTOFF_8H] = FAN_SHUTOFF_8H_DURATION_MS,
    [FAN_SHUTOFF_ALWAYS_ON] = 0,
};

static void publish_snapshot(const fan_control_snapshot_t *snapshot)
{
    uint32_t sequence = __atomic_add_fetch(&s_snapshot_sequence, 1, __ATOMIC_RELEASE);
    if ((sequence & 1U) == 0) {
        sequence = __atomic_add_fetch(&s_snapshot_sequence, 1, __ATOMIC_RELEASE);
    }
    s_snapshot = *snapshot;
    __atomic_store_n(&s_snapshot_sequence, sequence + 1U, __ATOMIC_RELEASE);

    diagnostics_flash_counters_t flash;
    diagnostics_get_flash_counters(&flash);
    s_coredump_snapshot.begin_sequence++;
    if ((s_coredump_snapshot.begin_sequence & 1U) == 0) {
        s_coredump_snapshot.begin_sequence++;
    }
    s_coredump_snapshot.version = 1;
    s_coredump_snapshot.size = sizeof(s_coredump_snapshot);
    s_coredump_snapshot.reset_reason = diagnostics_get_boot_info()->reset_reason;
    s_coredump_snapshot.uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_coredump_snapshot.control = *snapshot;
    s_coredump_snapshot.flash = flash;
    s_coredump_snapshot.free_heap_bytes = esp_get_free_heap_size();
    s_coredump_snapshot.minimum_free_heap_bytes = esp_get_minimum_free_heap_size();
    s_coredump_snapshot.end_sequence = s_coredump_snapshot.begin_sequence + 1U;
    s_coredump_snapshot.begin_sequence = s_coredump_snapshot.end_sequence;
}

esp_err_t fan_control_get_snapshot(fan_control_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        uint32_t before = __atomic_load_n(&s_snapshot_sequence, __ATOMIC_ACQUIRE);
        if (before & 1U) {
            continue;
        }
        *snapshot = s_snapshot;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint32_t after = __atomic_load_n(&s_snapshot_sequence, __ATOMIC_ACQUIRE);
        if (before == after && !(after & 1U)) {
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

static void cancel_timer(fan_control_snapshot_t *state)
{
    state->shutoff_mode = FAN_SHUTOFF_ALWAYS_ON;
    state->shutoff_active = false;
}

static void interpret_command(fan_control_snapshot_t *state, const fan_command_t *command,
                              TickType_t now, TickType_t *timer_started)
{
    state->accepted_sequence = command->sequence;
    switch (command->action) {
    case FAN_CONTROL_ACTION_CYCLE_NORMAL:
        state->desired_speed = (fan_speed_t)fan_policy_next_normal_speed(state->desired_speed);
        state->last_change_source = FAN_CONTROL_SOURCE_TOUCH;
        break;
    case FAN_CONTROL_ACTION_TOGGLE_TURBO:
        state->desired_speed = (fan_speed_t)fan_policy_toggle_turbo(state->desired_speed);
        state->last_change_source = FAN_CONTROL_SOURCE_TOUCH;
        break;
    case FAN_CONTROL_ACTION_CYCLE_TIMER:
        if (state->desired_speed != FAN_SPEED_OFF) {
            state->shutoff_mode = (fan_shutoff_mode_t)((state->shutoff_mode + 1) %
                                                       NUM_FAN_SHUTOFF_MODES);
            state->shutoff_active = state->shutoff_mode != FAN_SHUTOFF_ALWAYS_ON;
            *timer_started = now;
        }
        state->applied_sequence = command->sequence;
        return;
    case FAN_CONTROL_ACTION_SET_SPEED:
    default:
        state->desired_speed = command->speed;
        state->last_change_source = command->source;
        if (command->cancel_timer || command->speed == FAN_SPEED_OFF) {
            cancel_timer(state);
        }
        break;
    }
    if (state->desired_speed == FAN_SPEED_OFF) {
        cancel_timer(state);
    }
}

static bool timer_expired(fan_control_snapshot_t *state, TickType_t now,
                          TickType_t timer_started)
{
    if (!state->shutoff_active || state->desired_speed == FAN_SPEED_OFF) {
        return false;
    }
    uint32_t duration = s_shutoff_duration_ms[state->shutoff_mode];
    if (!fan_policy_tick_elapsed(now, timer_started, pdMS_TO_TICKS(duration))) {
        return false;
    }
    state->desired_speed = FAN_SPEED_OFF;
    state->last_change_source = FAN_CONTROL_SOURCE_TIMER;
    cancel_timer(state);
    return true;
}

static void update_transition_rate(fan_control_snapshot_t *state, int64_t now_us,
                                   int64_t *last_transition_us, int64_t *window_started_us,
                                   uint32_t *window_transitions)
{
    if (*last_transition_us != 0) {
        uint32_t interval_ms = (uint32_t)((now_us - *last_transition_us) / 1000);
        if (state->minimum_transition_interval_ms == 0 ||
            interval_ms < state->minimum_transition_interval_ms) {
            state->minimum_transition_interval_ms = interval_ms;
        }
    }
    *last_transition_us = now_us;
    if (*window_started_us == 0 || now_us - *window_started_us >= INT64_C(60000000)) {
        *window_started_us = now_us;
        *window_transitions = 0;
    }
    (*window_transitions)++;
    if (*window_transitions > state->maximum_transitions_per_minute) {
        state->maximum_transitions_per_minute = *window_transitions;
    }
}

static void fan_control_task(void *argument)
{
    (void)argument;
    fan_control_snapshot_t state = {
        .applied_speed = FAN_SPEED_OFF,
        .desired_speed = FAN_SPEED_OFF,
        .last_change_source = FAN_CONTROL_SOURCE_BOOT,
        .state = FAN_CONTROL_STATE_STARTING,
        .shutoff_mode = FAN_SHUTOFF_ALWAYS_ON,
    };
    TickType_t timer_started = 0;
    int64_t last_transition_us = 0;
    int64_t window_started_us = 0;
    uint32_t window_transitions = 0;
    esp_task_wdt_user_handle_t watchdog = NULL;

    int32_t retained_error = 0;
    uint32_t retained_phase = 0;
    diagnostics_get_fan_fault(&retained_error, &retained_phase);
    state.fault_latched = diagnostics_fan_fault_is_latched();
    state.fault_error = retained_error;
    state.fault_phase = (fan_transition_phase_t)retained_phase;

    s_init_result = fan_init();
    if (s_init_result != ESP_OK) {
        diagnostics_latch_fan_fault(s_init_result, FAN_TRANSITION_PHASE_DISABLE);
        state.fault_latched = true;
        state.fault_error = s_init_result;
        state.fault_phase = FAN_TRANSITION_PHASE_DISABLE;
    } else {
        fan_transition_phase_t phase = FAN_TRANSITION_PHASE_NONE;
        s_init_result = fan_transition(FAN_SPEED_OFF, &phase);
        if (s_init_result != ESP_OK) {
            diagnostics_latch_fan_fault(s_init_result, phase);
            state.fault_latched = true;
            state.fault_error = s_init_result;
            state.fault_phase = phase;
        }
    }
    if (s_init_result == ESP_OK) {
        s_init_result = esp_task_wdt_add_user("fan_control", &watchdog);
    }
    state.state = state.fault_latched ? FAN_CONTROL_STATE_FAULT_LATCHED :
                                       FAN_CONTROL_STATE_READY;
    publish_snapshot(&state);
    xEventGroupSetBits(s_events, EVENT_INITIALIZED);
    if (s_init_result != ESP_OK) {
        ESP_LOGE(TAG, "actor initialization failed: %s", esp_err_to_name(s_init_result));
        vTaskSuspend(NULL);
    }

    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(CONTROL_PERIOD_MS));
        int64_t cycle_started_us = esp_timer_get_time();
        TickType_t now = xTaskGetTickCount();
        uint32_t scheduling_lateness_us =
            (uint32_t)(now - last_wake) * (uint32_t)portTICK_PERIOD_MS * 1000U;
        state.control_cycles++;
        state.coalesced_commands = __atomic_load_n(&s_coalesced_commands, __ATOMIC_RELAXED);
        state.rejected_commands = __atomic_load_n(&s_rejected_commands, __ATOMIC_RELAXED);

        bool maintenance = __atomic_load_n(&s_maintenance_requested, __ATOMIC_ACQUIRE);
        if (maintenance && !state.fault_latched) {
            state.state = FAN_CONTROL_STATE_MAINTENANCE;
            state.maintenance_active = true;
            state.desired_speed = FAN_SPEED_OFF;
            state.last_change_source = FAN_CONTROL_SOURCE_MAINTENANCE;
            cancel_timer(&state);
        } else if (!maintenance && !state.fault_latched) {
            state.state = FAN_CONTROL_STATE_READY;
            state.maintenance_active = false;
            xEventGroupClearBits(s_events, EVENT_QUIESCED);
        }

        fan_command_t command;
        if (!state.fault_latched && !maintenance && xQueueReceive(s_mailbox, &command, 0) == pdTRUE) {
            interpret_command(&state, &command, now, &timer_started);
        }
        if (!state.fault_latched && !maintenance && timer_expired(&state, now, timer_started)) {
            diagnostics_record_event(DIAGNOSTICS_EVENT_TRANSITION_BEGIN,
                                     FAN_SPEED_OFF, FAN_CONTROL_SOURCE_TIMER);
        }

        if (!state.fault_latched &&
            fan_policy_transition_required(state.applied_speed, state.desired_speed)) {
            state.transitions_attempted++;
            diagnostics_record_event(DIAGNOSTICS_EVENT_TRANSITION_BEGIN,
                                     state.desired_speed, state.last_change_source);
            int64_t transition_started = esp_timer_get_time();
            fan_transition_phase_t phase = FAN_TRANSITION_PHASE_NONE;
            esp_err_t err = fan_transition(state.desired_speed, &phase);
            state.last_transition_us = (uint32_t)(esp_timer_get_time() - transition_started);
            if (err == ESP_OK) {
                state.applied_speed = state.desired_speed;
                state.applied_sequence = state.accepted_sequence;
                state.transitions_completed++;
                update_transition_rate(&state, transition_started, &last_transition_us,
                                       &window_started_us, &window_transitions);
                diagnostics_record_event(DIAGNOSTICS_EVENT_TRANSITION_COMMIT,
                                         state.applied_speed, state.applied_sequence);
            } else {
                state.applied_speed = FAN_SPEED_OFF;
                state.desired_speed = FAN_SPEED_OFF;
                state.fault_latched = true;
                state.state = FAN_CONTROL_STATE_FAULT_LATCHED;
                state.fault_error = err;
                state.fault_phase = phase;
                cancel_timer(&state);
                diagnostics_record_event(DIAGNOSTICS_EVENT_TRANSITION_FAILURE, phase, err);
                diagnostics_latch_fan_fault(err, phase);
                ESP_LOGE(TAG, "fan transition fault latched at %s: %s",
                         fan_control_phase_name(phase), esp_err_to_name(err));
            }
        } else if (!state.fault_latched && state.accepted_sequence != 0 &&
                   state.applied_sequence != state.accepted_sequence) {
            state.duplicate_commands++;
            state.applied_sequence = state.accepted_sequence;
        }

        state.pending = state.desired_speed != state.applied_speed ||
                        state.accepted_sequence != state.applied_sequence;
        state.stack_min_free_bytes =
            (uint32_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
        uint32_t cycle_us = (uint32_t)(esp_timer_get_time() - cycle_started_us);
        if (cycle_us > state.max_cycle_us) {
            state.max_cycle_us = cycle_us;
        }
        if (cycle_us > CONTROL_BUDGET_US) {
            state.budget_overruns++;
        }
        if (scheduling_lateness_us + cycle_us > CONTROL_DEADLINE_US) {
            state.deadline_misses++;
        }
        if (maintenance && state.applied_speed == FAN_SPEED_OFF && !state.fault_latched) {
            xEventGroupSetBits(s_events, EVENT_QUIESCED);
        }
        publish_snapshot(&state);
        if (watchdog != NULL) {
            esp_task_wdt_reset_user(watchdog);
        }
    }
}

esp_err_t fan_control_init(void)
{
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_mailbox = xQueueCreateStatic(1, sizeof(fan_command_t), s_mailbox_storage,
                                   &s_mailbox_buffer);
    s_events = xEventGroupCreateStatic(&s_event_buffer);
    if (s_mailbox == NULL || s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_snapshot = (fan_control_snapshot_t) {
        .applied_speed = FAN_SPEED_OFF,
        .desired_speed = FAN_SPEED_OFF,
        .state = FAN_CONTROL_STATE_STARTING,
        .shutoff_mode = FAN_SHUTOFF_ALWAYS_ON,
    };
    s_task = xTaskCreateStatic(fan_control_task, "fan_control", CONTROL_TASK_STACK_WORDS,
                               NULL, CONTROL_TASK_PRIORITY, s_task_stack, &s_task_buffer);
    if (s_task == NULL) {
        return ESP_ERR_NO_MEM;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, EVENT_INITIALIZED, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(INIT_TIMEOUT_MS));
    return (bits & EVENT_INITIALIZED) ? s_init_result : ESP_ERR_TIMEOUT;
}

static esp_err_t submit_command(fan_command_t *command, uint32_t *sequence)
{
    fan_control_snapshot_t state;
    if (s_task == NULL || fan_control_get_snapshot(&state) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (state.fault_latched) {
        __atomic_add_fetch(&s_rejected_commands, 1, __ATOMIC_RELAXED);
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_command_lock);
    if (__atomic_load_n(&s_maintenance_requested, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&s_rejected_commands, 1, __ATOMIC_RELAXED);
        portEXIT_CRITICAL(&s_command_lock);
        return ESP_ERR_NOT_ALLOWED;
    }
    uint32_t current;
    uint32_t next;
    do {
        current = __atomic_load_n(&s_command_sequence, __ATOMIC_RELAXED);
        next = fan_policy_next_sequence(current);
    } while (!__atomic_compare_exchange_n(&s_command_sequence, &current, next, false,
                                           __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    command->sequence = next;
    if (uxQueueMessagesWaiting(s_mailbox) != 0) {
        __atomic_add_fetch(&s_coalesced_commands, 1, __ATOMIC_RELAXED);
    }
    if (xQueueOverwrite(s_mailbox, command) != pdPASS) {
        __atomic_add_fetch(&s_rejected_commands, 1, __ATOMIC_RELAXED);
        portEXIT_CRITICAL(&s_command_lock);
        return ESP_FAIL;
    }
    portEXIT_CRITICAL(&s_command_lock);
    if (sequence != NULL) {
        *sequence = next;
    }
    return ESP_OK;
}

esp_err_t fan_control_submit_speed(fan_speed_t speed, fan_control_source_t source,
                                   bool cancel_timer_requested, uint32_t *sequence)
{
    if (speed < FAN_SPEED_OFF || speed >= NUM_FAN_SPEEDS) {
        return ESP_ERR_INVALID_ARG;
    }
    fan_command_t command = {
        .action = FAN_CONTROL_ACTION_SET_SPEED,
        .speed = speed,
        .source = source,
        .cancel_timer = cancel_timer_requested,
    };
    return submit_command(&command, sequence);
}

esp_err_t fan_control_submit_action(fan_control_action_t action, uint32_t *sequence)
{
    if (action <= FAN_CONTROL_ACTION_SET_SPEED || action > FAN_CONTROL_ACTION_CYCLE_TIMER) {
        return ESP_ERR_INVALID_ARG;
    }
    fan_command_t command = { .action = action, .source = FAN_CONTROL_SOURCE_TOUCH };
    return submit_command(&command, sequence);
}

esp_err_t fan_control_quiesce(TickType_t timeout)
{
    if (s_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_command_lock);
    __atomic_store_n(&s_maintenance_requested, true, __ATOMIC_RELEASE);
    if (uxQueueMessagesWaiting(s_mailbox) != 0) {
        __atomic_add_fetch(&s_coalesced_commands, 1, __ATOMIC_RELAXED);
    }
    // A command accepted immediately before maintenance must not replay if a
    // failed OTA/provisioning operation later releases the quiesce state.
    xQueueReset(s_mailbox);
    portEXIT_CRITICAL(&s_command_lock);
    diagnostics_record_event(DIAGNOSTICS_EVENT_MAINTENANCE_BEGIN, 0, 0);
    EventBits_t bits = xEventGroupWaitBits(s_events, EVENT_QUIESCED, pdFALSE, pdTRUE, timeout);
    if (!(bits & EVENT_QUIESCED)) {
        fan_control_end_maintenance();
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void fan_control_end_maintenance(void)
{
    bool was_active = __atomic_exchange_n(&s_maintenance_requested, false, __ATOMIC_ACQ_REL);
    if (was_active) {
        diagnostics_record_event(DIAGNOSTICS_EVENT_MAINTENANCE_END, 0, 0);
    }
}

const char *fan_control_state_name(fan_control_state_t state)
{
    switch (state) {
    case FAN_CONTROL_STATE_STARTING: return "starting";
    case FAN_CONTROL_STATE_READY: return "ready";
    case FAN_CONTROL_STATE_MAINTENANCE: return "maintenance";
    case FAN_CONTROL_STATE_FAULT_LATCHED: return "fault_latched";
    default: return "unknown";
    }
}

const char *fan_control_source_name(fan_control_source_t source)
{
    switch (source) {
    case FAN_CONTROL_SOURCE_BOOT: return "boot";
    case FAN_CONTROL_SOURCE_TOUCH: return "touch";
    case FAN_CONTROL_SOURCE_REST: return "rest";
    case FAN_CONTROL_SOURCE_TIMER: return "timer";
    case FAN_CONTROL_SOURCE_MAINTENANCE: return "maintenance";
    default: return "unknown";
    }
}

const char *fan_control_phase_name(fan_transition_phase_t phase)
{
    switch (phase) {
    case FAN_TRANSITION_PHASE_NONE: return "none";
    case FAN_TRANSITION_PHASE_DISABLE: return "disable";
    case FAN_TRANSITION_PHASE_ADDRESS: return "address";
    case FAN_TRANSITION_PHASE_ENABLE: return "enable";
    default: return "unknown";
    }
}

const char *fan_control_shutoff_mode_name(fan_shutoff_mode_t mode)
{
    switch (mode) {
    case FAN_SHUTOFF_2H: return "2h";
    case FAN_SHUTOFF_4H: return "4h";
    case FAN_SHUTOFF_8H: return "8h";
    case FAN_SHUTOFF_ALWAYS_ON: return "always_on";
    default: return "unknown";
    }
}
