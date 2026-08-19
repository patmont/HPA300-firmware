# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# Clean, exactly tagged commits use the tag. Development builds get a UTC
# configure stamp so consecutive dirty OTA images remain distinguishable.
if(DEFINED ENV{HPA300_VERSION} AND NOT "$ENV{HPA300_VERSION}" STREQUAL "")
    set(PROJECT_VER "$ENV{HPA300_VERSION}")
else()
    execute_process(
        COMMAND git describe --tags --exact-match --match "v[0-9]*" --dirty
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        RESULT_VARIABLE HPA300_TAG_RESULT
        OUTPUT_VARIABLE HPA300_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(HPA300_TAG_RESULT EQUAL 0 AND NOT HPA300_TAG MATCHES "-dirty$")
        set(PROJECT_VER "${HPA300_TAG}")
    else()
        execute_process(
            COMMAND git rev-parse --short=7 HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
            OUTPUT_VARIABLE HPA300_GIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(HPA300_GIT_HASH STREQUAL "")
            set(HPA300_GIT_HASH "unknown")
        endif()
        string(TIMESTAMP HPA300_BUILD_UTC "%Y%m%dT%H%M%SZ" UTC)
        set(PROJECT_VER "${HPA300_GIT_HASH}-dev-${HPA300_BUILD_UTC}")
    endif()
endif()
string(LENGTH "${PROJECT_VER}" HPA300_VERSION_LENGTH)
if(HPA300_VERSION_LENGTH GREATER 31)
    message(FATAL_ERROR "HPA300 firmware version exceeds the 31-character ESP-IDF limit: ${PROJECT_VER}")
endif()
message(STATUS "HPA300 firmware version: ${PROJECT_VER}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(HPA300-FIRMWARE)

# Reject signed release images that would leave less than 64 KiB for future
# growth in either 1.875 MiB OTA slot in the supported 4 MB layout.
add_custom_command(TARGET app POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DIMAGE=${CMAKE_BINARY_DIR}/${PROJECT_NAME}.bin
        -DPARTITION_SIZE=1966080
        -DMIN_HEADROOM=65536
        -P ${CMAKE_SOURCE_DIR}/cmake/check_ota_headroom.cmake
    VERBATIM
)
