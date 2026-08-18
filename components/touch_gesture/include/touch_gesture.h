#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TOUCH_GESTURE_KEY_COUNT 6

typedef struct {
    uint32_t active_mask;
    uint32_t candidate_started;
    uint32_t release_started;
    uint8_t candidate_key;
    bool armed;
    bool release_pending;
} touch_gesture_t;

void touch_gesture_init(touch_gesture_t *gesture);
void touch_gesture_edge(touch_gesture_t *gesture, uint8_t key, bool active,
                        uint32_t now_ms);
uint8_t touch_gesture_poll(touch_gesture_t *gesture, uint32_t now_ms);
void touch_gesture_discard_until_quiet(touch_gesture_t *gesture, uint32_t now_ms);
