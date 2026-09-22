if("${IDF_TARGET}" STREQUAL "linux")
    # Host builds get framebuffer and composition, which is where all
    # the layout decisions live. ssd1306.c is I2C transport only.
    idf_component_register(
        SRCS "oled_fb.c" "oled_screens.c"
        INCLUDE_DIRS "include"
        REQUIRES ui
    )
else()
    idf_component_register(
        SRCS "oled_fb.c" "oled_screens.c" "ssd1306.c"
        INCLUDE_DIRS "include"
        REQUIRES ui driver
    )
endif()
