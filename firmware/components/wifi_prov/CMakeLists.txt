if("${IDF_TARGET}" STREQUAL "linux")
    # Host builds get the session rules and credential checks, which is
    # where every security property lives. The SoftAP, HTTP server and
    # captive-portal DNS arrive in the next step and are target-only.
    idf_component_register(
        SRCS
        INCLUDE_DIRS "include"
    )
else()
    idf_component_register(
        SRCS "prov_logic.c"
        INCLUDE_DIRS "include"
    )
endif()
