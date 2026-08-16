#include <string.h>

#include "fan_transition_core.h"
#include "unity.h"

typedef struct {
    char calls[8];
    size_t count;
    fan_transition_phase_t fail_phase;
} fake_backend_t;

static esp_err_t fake_enable(void *context, bool enabled)
{
    fake_backend_t *fake = context;
    fake->calls[fake->count++] = enabled ? 'E' : 'D';
    if ((!enabled && fake->count == 1 && fake->fail_phase == FAN_TRANSITION_PHASE_DISABLE) ||
        (enabled && fake->fail_phase == FAN_TRANSITION_PHASE_ENABLE)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t fake_address(void *context, uint8_t address)
{
    fake_backend_t *fake = context;
    fake->calls[fake->count++] = (char)('0' + address);
    return fake->fail_phase == FAN_TRANSITION_PHASE_ADDRESS ? ESP_FAIL : ESP_OK;
}

static esp_err_t run(fake_backend_t *fake, fan_transition_phase_t *phase)
{
    fan_transition_backend_t backend = {
        .enable = fake_enable,
        .set_address = fake_address,
        .context = fake,
    };
    return fan_transition_execute(false, 3, &backend, phase);
}

TEST_CASE("active transition is one break-before-make transaction", "[fan_transition]")
{
    fake_backend_t fake = { 0 };
    fan_transition_phase_t phase;
    TEST_ASSERT_EQUAL(ESP_OK, run(&fake, &phase));
    TEST_ASSERT_EQUAL_STRING_LEN("D3E", fake.calls, fake.count);
    TEST_ASSERT_EQUAL(FAN_TRANSITION_PHASE_NONE, phase);
}

TEST_CASE("transition faults identify phase and finish disabled", "[fan_transition]")
{
    fake_backend_t disable = { .fail_phase = FAN_TRANSITION_PHASE_DISABLE };
    fan_transition_phase_t phase;
    TEST_ASSERT_EQUAL(ESP_FAIL, run(&disable, &phase));
    TEST_ASSERT_EQUAL(FAN_TRANSITION_PHASE_DISABLE, phase);
    TEST_ASSERT_EQUAL_STRING_LEN("DD", disable.calls, disable.count);

    fake_backend_t address = { .fail_phase = FAN_TRANSITION_PHASE_ADDRESS };
    TEST_ASSERT_EQUAL(ESP_FAIL, run(&address, &phase));
    TEST_ASSERT_EQUAL(FAN_TRANSITION_PHASE_ADDRESS, phase);
    TEST_ASSERT_EQUAL_STRING_LEN("D3D", address.calls, address.count);

    fake_backend_t enable = { .fail_phase = FAN_TRANSITION_PHASE_ENABLE };
    TEST_ASSERT_EQUAL(ESP_FAIL, run(&enable, &phase));
    TEST_ASSERT_EQUAL(FAN_TRANSITION_PHASE_ENABLE, phase);
    TEST_ASSERT_EQUAL_STRING_LEN("D3ED", enable.calls, enable.count);
}

TEST_CASE("off transition only disables the decoder", "[fan_transition]")
{
    fake_backend_t fake = { 0 };
    fan_transition_backend_t backend = { fake_enable, fake_address, &fake };
    fan_transition_phase_t phase;
    TEST_ASSERT_EQUAL(ESP_OK, fan_transition_execute(true, 0, &backend, &phase));
    TEST_ASSERT_EQUAL_STRING_LEN("D", fake.calls, fake.count);
}
