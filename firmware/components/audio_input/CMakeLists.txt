idf_component_register(
    SRCS "audio_input.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s
)
