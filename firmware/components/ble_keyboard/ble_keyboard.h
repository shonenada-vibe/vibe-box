#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*ble_keyboard_config_get_cb_t)(char *dst, size_t dst_len, void *ctx);
typedef esp_err_t (*ble_keyboard_config_set_cb_t)(const char *json, char *response, size_t response_len, void *ctx);

void ble_keyboard_set_config_callbacks(ble_keyboard_config_get_cb_t get_cb,
                                       ble_keyboard_config_set_cb_t set_cb,
                                       void *ctx);
esp_err_t ble_keyboard_init(void);
esp_err_t ble_keyboard_reinitialize(void);
bool ble_keyboard_is_connected(void);
bool ble_keyboard_text_client_connected(void);
bool ble_keyboard_text_notify_enabled(void);
bool ble_keyboard_config_notify_enabled(void);
esp_err_t ble_keyboard_send_key(uint8_t modifier, uint8_t keycode);
esp_err_t ble_keyboard_notify_text(const char *text);

#ifdef __cplusplus
}
#endif
