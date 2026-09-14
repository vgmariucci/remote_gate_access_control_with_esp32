idf_component_register(
    SRCS "rtc_ds3232.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_netif lwip
)
