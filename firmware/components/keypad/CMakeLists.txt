if("${IDF_TARGET}" STREQUAL "linux")
    # Host builds get only the pure logic. keypad.c is GPIO wiring and has
    # nothing testable in it that the logic layer does not already cover.
    idf_component_register(
        SRCS "keypad_logic.c"
        INCLUDE_DIRS "include"
    )
else()
    idf_component_register(
        SRCS "keypad.c" "keypad_logic.c"
        INCLUDE_DIRS "include"
        REQUIRES driver esp_timer
    )
endif()