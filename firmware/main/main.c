#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "vibe_box";

typedef enum {
    APP_STATE_BOOT = 0,
    APP_STATE_PROVISIONING,
    APP_STATE_IDLE,
    APP_STATE_RECORDING,
    APP_STATE_UPLOADING,
    APP_STATE_DISPLAYING,
    APP_STATE_ERROR,
} app_state_t;

static const char *app_state_name(app_state_t state)
{
    switch (state) {
    case APP_STATE_BOOT:
        return "boot";
    case APP_STATE_PROVISIONING:
        return "provisioning";
    case APP_STATE_IDLE:
        return "idle";
    case APP_STATE_RECORDING:
        return "recording";
    case APP_STATE_UPLOADING:
        return "uploading";
    case APP_STATE_DISPLAYING:
        return "displaying";
    case APP_STATE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

static void log_todo_modules(void)
{
    ESP_LOGI(TAG, "pending modules: ui_epaper, audio_input, net_client, sensors, storage");
}

static esp_err_t storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

void app_main(void)
{
    app_state_t state = APP_STATE_BOOT;

    ESP_LOGI(TAG, "vibe-box starting");
    ESP_LOGI(TAG, "state=%s", app_state_name(state));

    ESP_ERROR_CHECK(storage_init());
    ESP_LOGI(TAG, "NVS ready");

    log_todo_modules();

    state = APP_STATE_IDLE;
    ESP_LOGI(TAG, "state=%s", app_state_name(state));

    while (true) {
        ESP_LOGI(TAG, "heartbeat state=%s", app_state_name(state));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
