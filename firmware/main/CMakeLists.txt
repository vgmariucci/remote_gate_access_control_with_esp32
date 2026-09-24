idf_component_register(
    SRCS "app_main.c" "dev_console.c"
    INCLUDE_DIRS "."
    REQUIRES access_core ui gate_ctrl keypad lock_driver display_ssd1306
             rtc_ds3231 persist_store console mbedtls esp_timer driver
)
