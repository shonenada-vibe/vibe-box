idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES audio_input esp_event esp_http_client esp_http_server esp_netif esp_wifi json nvs_flash
)
