if("${IDF_TARGET}" STREQUAL "linux")
    # Only the retry policy is pure; the radio cannot run on the host.
    idf_component_register(
        SRCS "net_retry.c"
        INCLUDE_DIRS "include"
    )
else()
    idf_component_register(
        SRCS "net_retry.c" "net_link.c" "prov_nvs.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash lwip
    )
endif()
