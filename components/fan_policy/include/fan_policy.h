#pragma once

#include <stdbool.h>
#include <stdint.h>

bool fan_policy_transition_required(uint8_t applied, uint8_t desired);
uint8_t fan_policy_next_normal_speed(uint8_t speed);
uint8_t fan_policy_toggle_turbo(uint8_t speed);
bool fan_policy_tick_elapsed(uint32_t now, uint32_t started, uint32_t duration);
uint32_t fan_policy_next_sequence(uint32_t current);
