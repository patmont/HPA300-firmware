#include "touch_gesture.h"
#include "unity.h"

TEST_CASE("touch requires stable activation and release", "[touch_gesture]")
{
    touch_gesture_t gesture;
    touch_gesture_init(&gesture);
    touch_gesture_edge(&gesture, 1, true, 100);
    TEST_ASSERT_EQUAL_UINT8(0, touch_gesture_poll(&gesture, 139));
    TEST_ASSERT_EQUAL_UINT8(1, touch_gesture_poll(&gesture, 140));
    TEST_ASSERT_EQUAL_UINT8(0, touch_gesture_poll(&gesture, 500));
    touch_gesture_edge(&gesture, 1, false, 500);
    TEST_ASSERT_EQUAL_UINT8(0, touch_gesture_poll(&gesture, 619));
    TEST_ASSERT_EQUAL_UINT8(0, touch_gesture_poll(&gesture, 620));
    touch_gesture_edge(&gesture, 1, true, 700);
    TEST_ASSERT_EQUAL_UINT8(1, touch_gesture_poll(&gesture, 740));
}

TEST_CASE("brief and overlapping pads do not produce actions", "[touch_gesture]")
{
    touch_gesture_t gesture;
    touch_gesture_init(&gesture);
    touch_gesture_edge(&gesture, 2, true, 0);
    touch_gesture_edge(&gesture, 2, false, 20);
    TEST_ASSERT_EQUAL_UINT8(0, touch_gesture_poll(&gesture, 100));

    touch_gesture_edge(&gesture, 2, true, 200);
    touch_gesture_edge(&gesture, 3, true, 210);
    TEST_ASSERT_EQUAL_UINT8(0, touch_gesture_poll(&gesture, 300));
    touch_gesture_edge(&gesture, 2, false, 310);
    touch_gesture_edge(&gesture, 3, false, 320);
    TEST_ASSERT_EQUAL_UINT8(0, touch_gesture_poll(&gesture, 440));
    touch_gesture_edge(&gesture, 3, true, 500);
    TEST_ASSERT_EQUAL_UINT8(3, touch_gesture_poll(&gesture, 540));
}

TEST_CASE("queue overflow recovery cannot synthesize a press", "[touch_gesture]")
{
    touch_gesture_t gesture;
    touch_gesture_init(&gesture);
    touch_gesture_edge(&gesture, 4, true, 0);
    touch_gesture_discard_until_quiet(&gesture, 10);
    TEST_ASSERT_EQUAL_UINT8(0, touch_gesture_poll(&gesture, 130));
    TEST_ASSERT_EQUAL_UINT8(0, touch_gesture_poll(&gesture, 500));
    touch_gesture_edge(&gesture, 4, false, 510);
    touch_gesture_edge(&gesture, 4, true, 700);
    TEST_ASSERT_EQUAL_UINT8(4, touch_gesture_poll(&gesture, 740));
}
