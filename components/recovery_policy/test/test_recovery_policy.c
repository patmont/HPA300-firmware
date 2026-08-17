#include <limits.h>

#include "recovery_policy.h"
#include "unity.h"

TEST_CASE("service failures use bounded 1 5 30 60 second backoff", "[recovery]")
{
    recovery_policy_t policy = { 0 };
    const uint32_t expected[] = { 1, 5, 30, 60, 60 };
    uint32_t now = 100;
    for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        recovery_policy_mark_failed(&policy, now, 1000);
        TEST_ASSERT_FALSE(recovery_policy_is_due(&policy, now));
        now += expected[i] * 1000;
        TEST_ASSERT_TRUE(recovery_policy_is_due(&policy, now));
    }
}

TEST_CASE("all noncritical injected failures remain retryable", "[recovery][fault]")
{
    const char *faults[] = {
        "led", "touch", "http", "dns", "allocation", "queue",
        "ota_validation", "partial_initialization",
    };
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        recovery_policy_t policy = { 0 };
        recovery_policy_mark_failed(&policy, 0, 1000);
        TEST_ASSERT_FALSE_MESSAGE(policy.ready, faults[i]);
        TEST_ASSERT_TRUE_MESSAGE(recovery_policy_is_due(&policy, 1000), faults[i]);
        recovery_policy_mark_ready(&policy);
        TEST_ASSERT_TRUE_MESSAGE(policy.ready, faults[i]);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, policy.backoff_index, faults[i]);
    }
}

TEST_CASE("retry deadline comparison tolerates tick wrap", "[recovery]")
{
    recovery_policy_t policy = { 0 };
    recovery_policy_mark_failed(&policy, UINT32_MAX - 500U, 1000);
    TEST_ASSERT_FALSE(recovery_policy_is_due(&policy, UINT32_MAX - 1U));
    TEST_ASSERT_TRUE(recovery_policy_is_due(&policy, 499U));
}
