if("${IDF_TARGET}" STREQUAL "linux")
    # The wire format and the poll policy are pure and host-tested; the
    # HTTPS long poll is not.
    idf_component_register(
        SRCS "tg_proto.c" "tg_poll.c"
        INCLUDE_DIRS "include"
        REQUIRES access_core
    )
else()
    idf_component_register(
        SRCS "tg_proto.c" "tg_poll.c" "tg_client.c"
        INCLUDE_DIRS "include"
        REQUIRES access_core cred_check esp_http_client esp-tls json mbedtls nvs_flash
                 esp_timer
    )
endif()
