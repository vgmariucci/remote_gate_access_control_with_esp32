if("${IDF_TARGET}" STREQUAL "linux")
    # I2C register access end to end: nothing to test on the host.
    idf_component_register()
else()
    idf_component_register(
        SRCS "rtc_ds3231.c"
        INCLUDE_DIRS "include"
        REQUIRES driver
    )
endif()
