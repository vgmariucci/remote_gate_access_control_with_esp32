idf_component_register(
    SRCS "keypad.c" "keypad_logic.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_timer
)
