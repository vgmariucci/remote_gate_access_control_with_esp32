if("${IDF_TARGET}" STREQUAL "linux")
    # Nothing here is host-testable: it is I2C register access end to end.
    idf_component_register()
else()
    idf_component_register(
        SRCS "rtc_ds3232.c"
        INCLUDE_DIRS "include"
        REQUIRES driver esp_netif lwip
    )
endif()