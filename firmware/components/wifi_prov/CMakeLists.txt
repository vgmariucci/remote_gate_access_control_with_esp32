if("${IDF_TARGET}" STREQUAL "linux")
    # Host builds get the session rules and credential checks, which is
    # where every security property lives. The radio, the DNS responder
    # and the web server cannot run here.
    idf_component_register(
        SRCS "prov_logic.c"
        INCLUDE_DIRS "include"
    )
else()
    idf_component_register(
        SRCS "prov_logic.c" "wifi_prov.c" "prov_ap.c" "prov_dns.c" "prov_http.c"
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "."
        REQUIRES driver esp_wifi esp_netif esp_event esp_http_server esp_timer nvs_flash
    )
endif()
