#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool ready;
    uint8_t backoff_index;
    uint32_t retry_at;
} recovery_policy_t;

void recovery_policy_mark_failed(recovery_policy_t *policy, uint32_t now_ticks,
                                 uint32_t ticks_per_second);
bool recovery_policy_is_due(const recovery_policy_t *policy, uint32_t now_ticks);
void recovery_policy_mark_ready(recovery_policy_t *policy);
uint32_t recovery_policy_delay_seconds(const recovery_policy_t *policy);
