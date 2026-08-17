#include "recovery_policy.h"

#include <stddef.h>

static const uint32_t s_delays_seconds[] = { 1, 5, 30, 60 };

void recovery_policy_mark_failed(recovery_policy_t *policy, uint32_t now_ticks,
                                 uint32_t ticks_per_second)
{
    if (policy == NULL || ticks_per_second == 0) {
        return;
    }
    policy->ready = false;
    policy->retry_at = now_ticks +
        s_delays_seconds[policy->backoff_index] * ticks_per_second;
    if (policy->backoff_index + 1U <
        sizeof(s_delays_seconds) / sizeof(s_delays_seconds[0])) {
        policy->backoff_index++;
    }
}

bool recovery_policy_is_due(const recovery_policy_t *policy, uint32_t now_ticks)
{
    return policy != NULL && !policy->ready &&
           (int32_t)(now_ticks - policy->retry_at) >= 0;
}

void recovery_policy_mark_ready(recovery_policy_t *policy)
{
    if (policy != NULL) {
        policy->ready = true;
        policy->backoff_index = 0;
    }
}

uint32_t recovery_policy_delay_seconds(const recovery_policy_t *policy)
{
    if (policy == NULL) {
        return 0;
    }
    uint8_t index = policy->backoff_index;
    if (index >= sizeof(s_delays_seconds) / sizeof(s_delays_seconds[0])) {
        index = sizeof(s_delays_seconds) / sizeof(s_delays_seconds[0]) - 1U;
    }
    return s_delays_seconds[index];
}
