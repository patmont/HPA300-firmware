#include "fan_policy.h"

bool fan_policy_transition_required(uint8_t applied, uint8_t desired)
{
    return applied != desired;
}

uint8_t fan_policy_next_normal_speed(uint8_t speed)
{
    switch (speed) {
    case 0: return 1;
    case 1: return 2;
    case 2: return 3;
    default: return 0;
    }
}

uint8_t fan_policy_toggle_turbo(uint8_t speed)
{
    return speed == 4 ? 0 : 4;
}

bool fan_policy_tick_elapsed(uint32_t now, uint32_t started, uint32_t duration)
{
    return (uint32_t)(now - started) >= duration;
}

uint32_t fan_policy_next_sequence(uint32_t current)
{
    uint32_t next = current + 1U;
    return next == 0 ? 1U : next;
}
