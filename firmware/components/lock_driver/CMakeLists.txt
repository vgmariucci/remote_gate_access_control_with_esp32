if("${IDF_TARGET}" STREQUAL "linux")
    # Host builds get only the pure logic. lock_driver.c is GPIO wiring
    # and has nothing testable the logic layer does not already cover.
    idf_component_register(
        SRCS "lock_logic.c"
        INCLUDE_DIRS "include"
    )
else()
    idf_component_register(
        SRCS "lock_driver.c" "lock_logic.c"
        INCLUDE_DIRS "include"
        REQUIRES driver esp_timer
    )
endif()
