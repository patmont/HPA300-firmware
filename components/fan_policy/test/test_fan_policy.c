#include <limits.h>

#include "fan_policy.h"
#include "unity.h"

TEST_CASE("duplicate requests require no physical transition", "[fan_policy]")
{
    TEST_ASSERT_FALSE(fan_policy_transition_required(0, 0));
    TEST_ASSERT_FALSE(fan_policy_transition_required(4, 4));
    TEST_ASSERT_TRUE(fan_policy_transition_required(1, 2));
}

TEST_CASE("touch speed actions are deterministic", "[fan_policy]")
{
    TEST_ASSERT_EQUAL_UINT8(1, fan_policy_next_normal_speed(0));
    TEST_ASSERT_EQUAL_UINT8(2, fan_policy_next_normal_speed(1));
    TEST_ASSERT_EQUAL_UINT8(3, fan_policy_next_normal_speed(2));
    TEST_ASSERT_EQUAL_UINT8(0, fan_policy_next_normal_speed(3));
    TEST_ASSERT_EQUAL_UINT8(0, fan_policy_next_normal_speed(4));
    TEST_ASSERT_EQUAL_UINT8(4, fan_policy_toggle_turbo(0));
    TEST_ASSERT_EQUAL_UINT8(0, fan_policy_toggle_turbo(4));
}

TEST_CASE("tick and command sequence wrap are safe", "[fan_policy]")
{
    TEST_ASSERT_FALSE(fan_policy_tick_elapsed(2, UINT32_MAX - 2, 6));
    TEST_ASSERT_TRUE(fan_policy_tick_elapsed(3, UINT32_MAX - 2, 6));
    TEST_ASSERT_EQUAL_UINT32(1, fan_policy_next_sequence(UINT32_MAX));
}
