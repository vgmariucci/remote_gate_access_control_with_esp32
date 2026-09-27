if("${IDF_TARGET}" STREQUAL "linux")
    # Host builds get the pure logic, which carries every property worth
    # testing. persist_store.c is I2C transport only.
    idf_component_register(
        SRCS "persist_ring.c" "persist_codes.c"
        INCLUDE_DIRS "include"
        REQUIRES access_core
    )
else()
    idf_component_register(
        SRCS "persist_ring.c" "persist_codes.c" "persist_store.c"
        INCLUDE_DIRS "include"
        REQUIRES access_core driver
    )
endif()
