idf_component_register(
    SRCS "touch_input.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_i2c
)
