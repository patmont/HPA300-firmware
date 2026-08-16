if(NOT EXISTS "${IMAGE}")
    message(FATAL_ERROR "Signed OTA image was not generated: ${IMAGE}")
endif()

file(SIZE "${IMAGE}" IMAGE_SIZE)
math(EXPR MAX_IMAGE_SIZE "${PARTITION_SIZE} - ${MIN_HEADROOM}")
math(EXPR HEADROOM "${PARTITION_SIZE} - ${IMAGE_SIZE}")
if(IMAGE_SIZE GREATER MAX_IMAGE_SIZE)
    message(FATAL_ERROR
        "Signed OTA image is ${IMAGE_SIZE} bytes; ${HEADROOM} bytes remain, "
        "but at least ${MIN_HEADROOM} bytes are required")
endif()

message(STATUS "Signed OTA image headroom: ${HEADROOM} bytes")
