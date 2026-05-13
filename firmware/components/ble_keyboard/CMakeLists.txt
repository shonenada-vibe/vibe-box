idf_component_register(
    SRCS "ble_keyboard.c"
    INCLUDE_DIRS "."
    REQUIRES bt esp_hid nvs_flash
)
