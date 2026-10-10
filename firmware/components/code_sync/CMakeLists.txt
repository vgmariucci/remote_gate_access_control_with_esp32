if("${IDF_TARGET}" STREQUAL "linux")
    # The decisions and the parsing are pure and host-tested; the
    # WS2812 and the HTTPS client are not.
    idf_component_register(
        SRCS "sync_logic.c" "sync_parse.c" "status_led.c"
        INCLUDE_DIRS "include"
        REQUIRES access_core
    )
else()
    idf_component_register(
        SRCS "sync_logic.c" "sync_parse.c" "status_led.c" "status_led_rgb.c"
             "sync_client.c"
        INCLUDE_DIRS "include"
        REQUIRES access_core driver espressif__led_strip esp_http_client
                 esp-tls mbedtls nvs_flash
    )
endif()
