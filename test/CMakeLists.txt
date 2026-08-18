cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "../components")
set(TEST_COMPONENTS "board fan_policy fan_transition_core recovery_policy touch_gesture" CACHE STRING "Components whose tests are included")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(HPA300-FIRMWARE-tests)
