#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "fan_select.h"
#include "freertos/FreeRTOS.h"

typedef enum {
    FAN_CONTROL_SOURCE_BOOT = 0,
    FAN_CONTROL_SOURCE_TOUCH,
    FAN_CONTROL_SOURCE_REST,
    FAN_CONTROL_SOURCE_TIMER,
    FAN_CONTROL_SOURCE_MAINTENANCE,
} fan_control_source_t;

typedef enum {
    FAN_CONTROL_ACTION_SET_SPEED = 0,
    FAN_CONTROL_ACTION_CYCLE_NORMAL,
    FAN_CONTROL_ACTION_TOGGLE_TURBO,
    FAN_CONTROL_ACTION_CYCLE_TIMER,
} fan_control_action_t;

typedef enum {
    FAN_CONTROL_STATE_STARTING = 0,
    FAN_CONTROL_STATE_READY,
    FAN_CONTROL_STATE_MAINTENANCE,
    FAN_CONTROL_STATE_FAULT_LATCHED,
} fan_control_state_t;

typedef enum {
    FAN_SHUTOFF_2H = 0,
    FAN_SHUTOFF_4H,
    FAN_SHUTOFF_8H,
    FAN_SHUTOFF_ALWAYS_ON,
    NUM_FAN_SHUTOFF_MODES,
} fan_shutoff_mode_t;

typedef struct {
    fan_speed_t applied_speed;
    fan_speed_t desired_speed;
    fan_control_source_t last_change_source;
    fan_control_state_t state;
    fan_shutoff_mode_t shutoff_mode;
    bool shutoff_active;
    bool pending;
    bool fault_latched;
    bool maintenance_active;
    uint32_t accepted_sequence;
    uint32_t applied_sequence;
    uint32_t control_cycles;
    uint32_t max_cycle_us;
    uint32_t budget_overruns;
    uint32_t deadline_misses;
    uint32_t transitions_attempted;
    uint32_t transitions_completed;
    uint32_t duplicate_commands;
    uint32_t coalesced_commands;
    uint32_t rejected_commands;
    uint32_t last_transition_us;
    uint32_t minimum_transition_interval_ms;
    uint32_t maximum_transitions_per_minute;
    uint32_t stack_min_free_bytes;
    int32_t fault_error;
    fan_transition_phase_t fault_phase;
} fan_control_snapshot_t;

esp_err_t fan_control_init(void);
esp_err_t fan_control_submit_speed(fan_speed_t speed, fan_control_source_t source,
                                   bool cancel_timer, uint32_t *sequence);
esp_err_t fan_control_submit_action(fan_control_action_t action, uint32_t *sequence);
esp_err_t fan_control_get_snapshot(fan_control_snapshot_t *snapshot);

// Stops accepting public commands, requests OFF, and waits for the actor to
// confirm it. This is reserved for bounded flash-maintenance operations.
esp_err_t fan_control_quiesce(TickType_t timeout);
void fan_control_end_maintenance(void);

const char *fan_control_state_name(fan_control_state_t state);
const char *fan_control_source_name(fan_control_source_t source);
const char *fan_control_phase_name(fan_transition_phase_t phase);
const char *fan_control_shutoff_mode_name(fan_shutoff_mode_t mode);
