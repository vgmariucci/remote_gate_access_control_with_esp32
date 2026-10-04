if("${IDF_TARGET}" STREQUAL "linux")
    # Host builds get the session rules, the credential checks and the
    # form parser: everything that decides something, and everything
    # that reads bytes an attacker chose.
    idf_component_register(
        SRCS "prov_logic.c" "prov_form.c"
        INCLUDE_DIRS "include"
    )
else()
    # The radio belongs to net_link now; what is left here is the
    # portal: the session, the pages and the captive-portal DNS.
    idf_component_register(
        SRCS "prov_logic.c" "prov_form.c" "wifi_prov.c" "prov_ap.c" "prov_dns.c"
             "prov_http.c"
        INCLUDE_DIRS "include"
        PRIV_INCLUDE_DIRS "."
        REQUIRES driver esp_http_server esp_timer net_link
    )
endif()
