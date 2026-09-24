if("${IDF_TARGET}" STREQUAL "linux")
    # Host builds get the ring logic, which carries every property worth
    # testing. persist_store.c is I2C transport only.
    idf_component_register(
        SRCS "persist_ring.c"
        INCLUDE_DIRS "include"
    )
else()
    idf_component_register(
        SRCS "persist_ring.c" "persist_store.c"
        INCLUDE_DIRS "include"
        REQUIRES driver
    )
endif()
