idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES audio_input ble_keyboard ui_epaper esp_driver_gpio esp_event esp_http_client esp_http_server esp_netif esp_timer esp_wifi json mbedtls nvs_flash
)
