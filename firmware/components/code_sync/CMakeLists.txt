if("${IDF_TARGET}" STREQUAL "linux")
    # The decisions are pure and host-tested; the WS2812 is not.
    idf_component_register(
        SRCS "sync_logic.c" "status_led.c"
        INCLUDE_DIRS "include"
        REQUIRES access_core
    )
else()
    idf_component_register(
        SRCS "sync_logic.c" "status_led.c" "status_led_rgb.c"
        INCLUDE_DIRS "include"
        REQUIRES access_core driver espressif__led_strip
    )
endif()
