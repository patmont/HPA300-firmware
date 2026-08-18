#include "touch_gesture.h"

#include <stddef.h>

#define ACTIVE_STABLE_MS 40U
#define RELEASE_STABLE_MS 120U

static bool elapsed(uint32_t now, uint32_t started, uint32_t interval)
{
    return (uint32_t)(now - started) >= interval;
}

void touch_gesture_init(touch_gesture_t *gesture)
{
    if (gesture != NULL) {
        *gesture = (touch_gesture_t) { .armed = true };
    }
}

void touch_gesture_edge(touch_gesture_t *gesture, uint8_t key, bool active,
                        uint32_t now_ms)
{
    if (gesture == NULL || key < 1 || key > TOUCH_GESTURE_KEY_COUNT) {
        return;
    }
    uint32_t bit = UINT32_C(1) << (key - 1U);
    if (active) {
        if (gesture->active_mask & bit) {
            return;
        }
        gesture->active_mask |= bit;
        gesture->release_pending = false;
        if (!gesture->armed) {
            return;
        }
        if (gesture->active_mask == bit && gesture->candidate_key == 0) {
            gesture->candidate_key = key;
            gesture->candidate_started = now_ms;
        } else {
            // More than one pad in the same gesture is ambiguous. Discard it
            // and rearm as soon as every pad has been stably released.
            gesture->candidate_key = 0;
            gesture->armed = false;
        }
        return;
    }

    gesture->active_mask &= ~bit;
    if (gesture->armed && gesture->candidate_key == key) {
        gesture->candidate_key = 0;
    }
    if (!gesture->armed && gesture->active_mask == 0) {
        gesture->release_pending = true;
        gesture->release_started = now_ms;
    }
}

uint8_t touch_gesture_poll(touch_gesture_t *gesture, uint32_t now_ms)
{
    if (gesture == NULL) {
        return 0;
    }
    if (!gesture->armed) {
        if (gesture->release_pending && gesture->active_mask == 0 &&
            elapsed(now_ms, gesture->release_started, RELEASE_STABLE_MS)) {
            gesture->armed = true;
            gesture->release_pending = false;
        }
        return 0;
    }
    if (gesture->candidate_key == 0 ||
        gesture->active_mask != (UINT32_C(1) << (gesture->candidate_key - 1U)) ||
        !elapsed(now_ms, gesture->candidate_started, ACTIVE_STABLE_MS)) {
        return 0;
    }
    uint8_t accepted = gesture->candidate_key;
    gesture->candidate_key = 0;
    gesture->armed = false;
    gesture->release_pending = false;
    return accepted;
}

void touch_gesture_discard_until_quiet(touch_gesture_t *gesture, uint32_t now_ms)
{
    if (gesture != NULL) {
        *gesture = (touch_gesture_t) {
            .armed = false,
            .release_pending = true,
            .release_started = now_ms,
        };
    }
}
