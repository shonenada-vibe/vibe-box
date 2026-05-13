#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_keyboard_init(void);
esp_err_t ble_keyboard_reinitialize(void);
bool ble_keyboard_is_connected(void);
esp_err_t ble_keyboard_send_key(uint8_t modifier, uint8_t keycode);
esp_err_t ble_keyboard_notify_text(const char *text);

#ifdef __cplusplus
}
#endif
