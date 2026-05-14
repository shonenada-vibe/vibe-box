#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "audio_input.h"
#include "ble_keyboard.h"
#include "touch_input.h"
#include "ui_epaper.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "vibe_box";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define QUERY_RESPONSE_BUFFER_SIZE      16384
#define FORM_BODY_BUFFER_SIZE           1024
#define URL_BUFFER_SIZE                 320
#define QUERY_DISPLAY_LINE_MAX          4
#define QUERY_DISPLAY_COL_MAX           96
#define QUERY_TEXT_MAX                  256
#define QUERY_REPLY_MAX                 512
#define QUERY_REQUEST_ID_MAX            64
#define QUERY_DISPLAY_BITMAP_BYTES      UI_EPAPER_FRAME_BUFFER_BYTES
#define MULTIPART_BOUNDARY              "----VibeBoxBoundary7MA4YWxkTrZu0gW"
#define MAIN_LOOP_IDLE_MS               10000U
#define UI_DASHBOARD_REFRESH_MS         10000U
#define RUNTIME_WIFI_SSID_MAX           33
#define RUNTIME_WIFI_PASSWORD_MAX       65
#define RUNTIME_SERVER_BASE_URL_MAX     192
#define RUNTIME_API_TOKEN_MAX           192
#define RUNTIME_WHISPER_API_URL_MAX     256
#define RUNTIME_WHISPER_API_KEY_MAX     256
#define RUNTIME_OPENAI_API_BASE_MAX     256
#define RUNTIME_OPENAI_API_KEY_MAX      256
#define RUNTIME_STT_MODEL_MAX           96
#define RUNTIME_TRANSLATION_MODEL_MAX   96
#define RUNTIME_TRANSLATION_LANGUAGE_MAX 32
#define RUNTIME_TRANSLATION_PROMPT_MAX  512
#define RUNTIME_REFINE_PROMPT_MAX       512
#define RUNTIME_DEVICE_ID_MAX           64
#define RUNTIME_FIRMWARE_VERSION_MAX    64
#define RUNTIME_LANGUAGE_MAX            16
#define PROVISIONING_FORM_BUFFER_SIZE   4096
#define PROVISIONING_HTML_BUFFER_SIZE   8192
#define PROVISIONING_STATUS_BUFFER_SIZE 2048
#define WIFI_CONNECT_TIMEOUT_MS         30000
#define NVS_NAMESPACE                   "vibe_box"

#ifdef CONFIG_VIBE_BOX_API_TOKEN
#define VIBE_BOX_DEFAULT_API_TOKEN CONFIG_VIBE_BOX_API_TOKEN
#else
#define VIBE_BOX_DEFAULT_API_TOKEN ""
#endif

#ifdef CONFIG_VIBE_BOX_WHISPER_API_URL
#define VIBE_BOX_DEFAULT_WHISPER_API_URL CONFIG_VIBE_BOX_WHISPER_API_URL
#else
#define VIBE_BOX_DEFAULT_WHISPER_API_URL ""
#endif

#ifdef CONFIG_VIBE_BOX_WHISPER_API_KEY
#define VIBE_BOX_DEFAULT_WHISPER_API_KEY CONFIG_VIBE_BOX_WHISPER_API_KEY
#else
#define VIBE_BOX_DEFAULT_WHISPER_API_KEY ""
#endif

#ifdef CONFIG_VIBE_BOX_STT_MODEL
#define VIBE_BOX_DEFAULT_STT_MODEL CONFIG_VIBE_BOX_STT_MODEL
#else
#define VIBE_BOX_DEFAULT_STT_MODEL "whisper-large-v3-turbo"
#endif

#ifdef CONFIG_VIBE_BOX_OPENAI_API_BASE
#define VIBE_BOX_DEFAULT_OPENAI_API_BASE CONFIG_VIBE_BOX_OPENAI_API_BASE
#else
#define VIBE_BOX_DEFAULT_OPENAI_API_BASE ""
#endif

#ifdef CONFIG_VIBE_BOX_OPENAI_API_KEY
#define VIBE_BOX_DEFAULT_OPENAI_API_KEY CONFIG_VIBE_BOX_OPENAI_API_KEY
#else
#define VIBE_BOX_DEFAULT_OPENAI_API_KEY ""
#endif

#ifdef CONFIG_VIBE_BOX_TRANSLATION_MODEL
#define VIBE_BOX_DEFAULT_TRANSLATION_MODEL CONFIG_VIBE_BOX_TRANSLATION_MODEL
#else
#define VIBE_BOX_DEFAULT_TRANSLATION_MODEL "gpt-4o-mini"
#endif

#ifdef CONFIG_VIBE_BOX_TRANSLATION_TARGET_LANGUAGE
#define VIBE_BOX_DEFAULT_TRANSLATION_TARGET_LANGUAGE CONFIG_VIBE_BOX_TRANSLATION_TARGET_LANGUAGE
#else
#define VIBE_BOX_DEFAULT_TRANSLATION_TARGET_LANGUAGE "English"
#endif

#ifdef CONFIG_VIBE_BOX_TRANSLATION_PROMPT
#define VIBE_BOX_DEFAULT_TRANSLATION_PROMPT CONFIG_VIBE_BOX_TRANSLATION_PROMPT
#else
#define VIBE_BOX_DEFAULT_TRANSLATION_PROMPT "You are a translation engine. Translate the user text to the target language. Return only the translated text."
#endif

#ifdef CONFIG_VIBE_BOX_REFINE_PROMPT
#define VIBE_BOX_DEFAULT_REFINE_PROMPT CONFIG_VIBE_BOX_REFINE_PROMPT
#else
#define VIBE_BOX_DEFAULT_REFINE_PROMPT "You are a text refinement engine. Rewrite the text to be fluent and natural while preserving the user's final intent. Remove filler words, repeated phrases, false starts, and self-corrections. If the speaker corrects themselves, keep only the corrected final meaning. Return only the refined text."
#endif

#ifdef CONFIG_VIBE_BOX_LANGUAGE
#define VIBE_BOX_DEFAULT_LANGUAGE CONFIG_VIBE_BOX_LANGUAGE
#else
#define VIBE_BOX_DEFAULT_LANGUAGE "zh"
#endif

#ifdef CONFIG_VIBE_BOX_RECORDING_DURATION_MS
#define VIBE_BOX_DEFAULT_RECORDING_DURATION_MS CONFIG_VIBE_BOX_RECORDING_DURATION_MS
#else
#define VIBE_BOX_DEFAULT_RECORDING_DURATION_MS 3000U
#endif

#ifdef CONFIG_VIBE_BOX_ENABLE_I2S_CAPTURE
#define VIBE_BOX_ENABLE_I2S_CAPTURE 1
#else
#define VIBE_BOX_ENABLE_I2S_CAPTURE 0
#endif

#ifdef CONFIG_VIBE_BOX_I2S_PORT
#define VIBE_BOX_I2S_PORT CONFIG_VIBE_BOX_I2S_PORT
#else
#define VIBE_BOX_I2S_PORT 0
#endif

#ifdef CONFIG_VIBE_BOX_I2C_PORT
#define VIBE_BOX_I2C_PORT CONFIG_VIBE_BOX_I2C_PORT
#else
#define VIBE_BOX_I2C_PORT 0
#endif

#ifdef CONFIG_VIBE_BOX_PWR_BUTTON_GPIO
#define VIBE_BOX_PWR_BUTTON_GPIO CONFIG_VIBE_BOX_PWR_BUTTON_GPIO
#else
#define VIBE_BOX_PWR_BUTTON_GPIO 18
#endif

#ifdef CONFIG_VIBE_BOX_PWR_DOUBLE_CLICK_MS
#define VIBE_BOX_PWR_DOUBLE_CLICK_MS CONFIG_VIBE_BOX_PWR_DOUBLE_CLICK_MS
#else
#define VIBE_BOX_PWR_DOUBLE_CLICK_MS 500
#endif

#ifdef CONFIG_VIBE_BOX_PWR_LONG_PRESS_MS
#define VIBE_BOX_PWR_LONG_PRESS_MS CONFIG_VIBE_BOX_PWR_LONG_PRESS_MS
#else
#define VIBE_BOX_PWR_LONG_PRESS_MS 5000
#endif

#ifdef CONFIG_VIBE_BOX_BOOT_RESTART_LONG_PRESS_MS
#define VIBE_BOX_BOOT_RESTART_LONG_PRESS_MS CONFIG_VIBE_BOX_BOOT_RESTART_LONG_PRESS_MS
#else
#define VIBE_BOX_BOOT_RESTART_LONG_PRESS_MS 3000
#endif

#ifdef CONFIG_VIBE_BOX_I2C_SDA_GPIO
#define VIBE_BOX_I2C_SDA_GPIO CONFIG_VIBE_BOX_I2C_SDA_GPIO
#else
#define VIBE_BOX_I2C_SDA_GPIO 47
#endif

#ifdef CONFIG_VIBE_BOX_I2C_SCL_GPIO
#define VIBE_BOX_I2C_SCL_GPIO CONFIG_VIBE_BOX_I2C_SCL_GPIO
#else
#define VIBE_BOX_I2C_SCL_GPIO 48
#endif

#ifdef CONFIG_VIBE_BOX_CODEC_I2C_ADDR
#define VIBE_BOX_CODEC_I2C_ADDR CONFIG_VIBE_BOX_CODEC_I2C_ADDR
#else
#define VIBE_BOX_CODEC_I2C_ADDR 0x18
#endif

#ifdef CONFIG_VIBE_BOX_AUDIO_PA_ENABLE_GPIO
#define VIBE_BOX_AUDIO_PA_ENABLE_GPIO CONFIG_VIBE_BOX_AUDIO_PA_ENABLE_GPIO
#else
#define VIBE_BOX_AUDIO_PA_ENABLE_GPIO 42
#endif

#ifdef CONFIG_VIBE_BOX_AUDIO_PA_CONTROL_GPIO
#define VIBE_BOX_AUDIO_PA_CONTROL_GPIO CONFIG_VIBE_BOX_AUDIO_PA_CONTROL_GPIO
#else
#define VIBE_BOX_AUDIO_PA_CONTROL_GPIO 46
#endif

#ifdef CONFIG_VIBE_BOX_I2S_MCLK_GPIO
#define VIBE_BOX_I2S_MCLK_GPIO CONFIG_VIBE_BOX_I2S_MCLK_GPIO
#else
#define VIBE_BOX_I2S_MCLK_GPIO -1
#endif

#ifdef CONFIG_VIBE_BOX_I2S_BCLK_GPIO
#define VIBE_BOX_I2S_BCLK_GPIO CONFIG_VIBE_BOX_I2S_BCLK_GPIO
#else
#define VIBE_BOX_I2S_BCLK_GPIO -1
#endif

#ifdef CONFIG_VIBE_BOX_I2S_WS_GPIO
#define VIBE_BOX_I2S_WS_GPIO CONFIG_VIBE_BOX_I2S_WS_GPIO
#else
#define VIBE_BOX_I2S_WS_GPIO -1
#endif

#ifdef CONFIG_VIBE_BOX_I2S_DIN_GPIO
#define VIBE_BOX_I2S_DIN_GPIO CONFIG_VIBE_BOX_I2S_DIN_GPIO
#else
#define VIBE_BOX_I2S_DIN_GPIO -1
#endif

#ifdef CONFIG_VIBE_BOX_I2S_SAMPLE_RATE_HZ
#define VIBE_BOX_I2S_SAMPLE_RATE_HZ CONFIG_VIBE_BOX_I2S_SAMPLE_RATE_HZ
#else
#define VIBE_BOX_I2S_SAMPLE_RATE_HZ 16000
#endif

#ifdef CONFIG_VIBE_BOX_I2S_CHANNELS
#define VIBE_BOX_I2S_CHANNELS CONFIG_VIBE_BOX_I2S_CHANNELS
#else
#define VIBE_BOX_I2S_CHANNELS 1
#endif

#ifdef CONFIG_VIBE_BOX_TOUCH_ENABLE
#define VIBE_BOX_TOUCH_ENABLE 1
#else
#define VIBE_BOX_TOUCH_ENABLE 0
#endif

#ifdef CONFIG_VIBE_BOX_TOUCH_RST_GPIO
#define VIBE_BOX_TOUCH_RST_GPIO CONFIG_VIBE_BOX_TOUCH_RST_GPIO
#else
#define VIBE_BOX_TOUCH_RST_GPIO 7
#endif

#ifdef CONFIG_VIBE_BOX_TOUCH_INT_GPIO
#define VIBE_BOX_TOUCH_INT_GPIO CONFIG_VIBE_BOX_TOUCH_INT_GPIO
#else
#define VIBE_BOX_TOUCH_INT_GPIO 21
#endif

#ifdef CONFIG_VIBE_BOX_TOUCH_I2C_ADDR
#define VIBE_BOX_TOUCH_I2C_ADDR CONFIG_VIBE_BOX_TOUCH_I2C_ADDR
#else
#define VIBE_BOX_TOUCH_I2C_ADDR 0x38
#endif

#ifdef CONFIG_VIBE_BOX_TOUCH_POLL_MS
#define VIBE_BOX_TOUCH_POLL_MS CONFIG_VIBE_BOX_TOUCH_POLL_MS
#else
#define VIBE_BOX_TOUCH_POLL_MS 20
#endif

#ifdef CONFIG_VIBE_BOX_TOUCH_DOUBLE_TAP_MS
#define VIBE_BOX_TOUCH_DOUBLE_TAP_MS CONFIG_VIBE_BOX_TOUCH_DOUBLE_TAP_MS
#else
#define VIBE_BOX_TOUCH_DOUBLE_TAP_MS 500
#endif

#ifdef CONFIG_VIBE_BOX_TOUCH_TAP_MAX_MS
#define VIBE_BOX_TOUCH_TAP_MAX_MS CONFIG_VIBE_BOX_TOUCH_TAP_MAX_MS
#else
#define VIBE_BOX_TOUCH_TAP_MAX_MS 300
#endif

#ifdef CONFIG_VIBE_BOX_TOUCH_HOLD_START_MS
#define VIBE_BOX_TOUCH_HOLD_START_MS CONFIG_VIBE_BOX_TOUCH_HOLD_START_MS
#else
#define VIBE_BOX_TOUCH_HOLD_START_MS 180
#endif

#ifdef CONFIG_VIBE_BOX_RECORDING_MAX_MS
#define VIBE_BOX_RECORDING_MAX_MS CONFIG_VIBE_BOX_RECORDING_MAX_MS
#else
#define VIBE_BOX_RECORDING_MAX_MS 60000U
#endif

/* BOOT button on ESP32-S3 is typically wired to GPIO 0 and reads active-low
 * with the internal pull-up enabled. */
#define VIBE_BOX_RECORD_BUTTON_GPIO       0
#define VIBE_BOX_RECORDING_MIN_MS         300U
#define VIBE_BOX_RECORDING_TIMEOUT_GRACE_MS 1000U
#define VIBE_BOX_BUTTON_POLL_MS           20
#define VIBE_BOX_BUTTON_DEBOUNCE_SAMPLES  3
#define VIBE_BOX_TOUCH_POST_ENTER_IGNORE_MS 250U
#define BLE_HID_KEY_ENTER                 0x28U
#define VIBE_BOX_BATTERY_ADC_UNIT         ADC_UNIT_1
#define VIBE_BOX_BATTERY_ADC_CHANNEL      ADC_CHANNEL_3
#define VIBE_BOX_BATTERY_ADC_ATTEN        ADC_ATTEN_DB_12
#define VIBE_BOX_BATTERY_EMPTY_MV         3000
#define VIBE_BOX_BATTERY_FULL_MV          4120
#define VIBE_BOX_BATTERY_DIVIDER_NUM      2
#define VIBE_BOX_BATTERY_SAMPLE_COUNT     8

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static httpd_handle_t s_provisioning_server;
static int s_retry_num;
static bool s_network_stack_ready;
static bool s_wifi_driver_ready;
static bool s_wifi_station_started;
static bool s_provisioning_reconnect_requested;
static bool s_runtime_config_reconnect_requested;

typedef enum {
    APP_STATE_BOOT = 0,
    APP_STATE_PROVISIONING,
    APP_STATE_IDLE,
    APP_STATE_RECORDING,
    APP_STATE_UPLOADING,
    APP_STATE_DISPLAYING,
    APP_STATE_ERROR,
} app_state_t;

typedef struct {
    char wifi_ssid[RUNTIME_WIFI_SSID_MAX];
    char wifi_password[RUNTIME_WIFI_PASSWORD_MAX];
    char server_base_url[RUNTIME_SERVER_BASE_URL_MAX];
    char api_token[RUNTIME_API_TOKEN_MAX];
    char whisper_api_url[RUNTIME_WHISPER_API_URL_MAX];
    char whisper_api_key[RUNTIME_WHISPER_API_KEY_MAX];
    char stt_model[RUNTIME_STT_MODEL_MAX];
    char openai_api_base[RUNTIME_OPENAI_API_BASE_MAX];
    char openai_api_key[RUNTIME_OPENAI_API_KEY_MAX];
    char translation_model[RUNTIME_TRANSLATION_MODEL_MAX];
    char translation_target_language[RUNTIME_TRANSLATION_LANGUAGE_MAX];
    char translation_prompt[RUNTIME_TRANSLATION_PROMPT_MAX];
    char refine_prompt[RUNTIME_REFINE_PROMPT_MAX];
    bool translation_enabled;
    bool refine_enabled;
    char device_id[RUNTIME_DEVICE_ID_MAX];
    char firmware_version[RUNTIME_FIRMWARE_VERSION_MAX];
    char language[RUNTIME_LANGUAGE_MAX];
    uint32_t recording_duration_ms;
} runtime_config_t;

typedef struct {
    char request_id[QUERY_REQUEST_ID_MAX];
    char transcript[QUERY_TEXT_MAX];
    char reply_text[QUERY_REPLY_MAX];
    char display_lines[QUERY_DISPLAY_LINE_MAX][QUERY_DISPLAY_COL_MAX];
    size_t display_line_count;
    bool has_display_bitmap;
    uint8_t display_bitmap[QUERY_DISPLAY_BITMAP_BYTES];
} query_result_t;

typedef struct {
    app_state_t page_state;
    char headline[64];
    char detail[QUERY_REPLY_MAX];
    char lines[QUERY_DISPLAY_LINE_MAX][QUERY_DISPLAY_COL_MAX];
    size_t line_count;
} ui_snapshot_t;

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
    bool truncated;
} http_response_context_t;

typedef enum {
    RECORD_OWNER_NONE = 0,
    RECORD_OWNER_TOUCH,
} record_owner_t;

static runtime_config_t s_runtime_config;
static query_result_t s_last_query_result;
static ui_snapshot_t s_ui_snapshot;
static uint32_t s_press_start_ms;
static uint32_t s_touch_press_start_ms;
static uint32_t s_touch_last_tap_release_ms;
static uint32_t s_touch_ignore_until_ms;
static volatile record_owner_t s_record_owner;
static volatile bool s_touch_recording_started;
static volatile bool s_touch_release_pending;
static volatile bool s_touch_abort_pending;
static bool s_touch_ignoring_current_press;
static TaskHandle_t s_ui_dashboard_task_handle;
static adc_oneshot_unit_handle_t s_battery_adc_handle;
static adc_cali_handle_t s_battery_adc_cali_handle;
static bool s_battery_adc_ready;
static bool s_battery_adc_calibrated;

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

static void set_state(app_state_t *state, app_state_t next_state, const char *reason)
{
    if (*state == next_state) {
        ESP_LOGI(TAG, "state=%s reason=%s", app_state_name(next_state), reason);
        return;
    }

    ESP_LOGI(TAG,
             "state transition: %s -> %s reason=%s",
             app_state_name(*state),
             app_state_name(next_state),
             reason);
    *state = next_state;
}

static void runtime_config_load_defaults(runtime_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strlcpy(cfg->wifi_ssid, CONFIG_VIBE_BOX_WIFI_SSID, sizeof(cfg->wifi_ssid));
    strlcpy(cfg->wifi_password, CONFIG_VIBE_BOX_WIFI_PASSWORD, sizeof(cfg->wifi_password));
    strlcpy(cfg->server_base_url,
            CONFIG_VIBE_BOX_SERVER_BASE_URL,
            sizeof(cfg->server_base_url));
    strlcpy(cfg->api_token, VIBE_BOX_DEFAULT_API_TOKEN, sizeof(cfg->api_token));
    strlcpy(cfg->whisper_api_url,
            VIBE_BOX_DEFAULT_WHISPER_API_URL,
            sizeof(cfg->whisper_api_url));
    strlcpy(cfg->whisper_api_key,
            VIBE_BOX_DEFAULT_WHISPER_API_KEY,
            sizeof(cfg->whisper_api_key));
    strlcpy(cfg->stt_model, VIBE_BOX_DEFAULT_STT_MODEL, sizeof(cfg->stt_model));
    strlcpy(cfg->openai_api_base,
            VIBE_BOX_DEFAULT_OPENAI_API_BASE,
            sizeof(cfg->openai_api_base));
    strlcpy(cfg->openai_api_key,
            VIBE_BOX_DEFAULT_OPENAI_API_KEY,
            sizeof(cfg->openai_api_key));
    strlcpy(cfg->translation_model,
            VIBE_BOX_DEFAULT_TRANSLATION_MODEL,
            sizeof(cfg->translation_model));
    strlcpy(cfg->translation_target_language,
            VIBE_BOX_DEFAULT_TRANSLATION_TARGET_LANGUAGE,
            sizeof(cfg->translation_target_language));
    strlcpy(cfg->translation_prompt,
            VIBE_BOX_DEFAULT_TRANSLATION_PROMPT,
            sizeof(cfg->translation_prompt));
    strlcpy(cfg->refine_prompt,
            VIBE_BOX_DEFAULT_REFINE_PROMPT,
            sizeof(cfg->refine_prompt));
    cfg->translation_enabled = false;
    cfg->refine_enabled = false;
    strlcpy(cfg->device_id, CONFIG_VIBE_BOX_DEVICE_ID, sizeof(cfg->device_id));
    strlcpy(cfg->firmware_version,
            CONFIG_VIBE_BOX_FIRMWARE_VERSION,
            sizeof(cfg->firmware_version));
    strlcpy(cfg->language, VIBE_BOX_DEFAULT_LANGUAGE, sizeof(cfg->language));
    cfg->recording_duration_ms = VIBE_BOX_DEFAULT_RECORDING_DURATION_MS;
}

static bool runtime_config_is_complete(const runtime_config_t *cfg)
{
    return cfg->wifi_ssid[0] != '\0' && cfg->whisper_api_url[0] != '\0';
}

static void log_runtime_config(const runtime_config_t *cfg, const char *source)
{
    ESP_LOGI(TAG, "firmware startup diagnostics");
    ESP_LOGI(TAG, "  config_source=%s", source);
    ESP_LOGI(TAG, "  project=vibe_box");
    ESP_LOGI(TAG, "  free_heap=%" PRIu32, esp_get_free_heap_size());
    ESP_LOGI(TAG, "  wifi_ssid=%s", cfg->wifi_ssid[0] ? cfg->wifi_ssid : "<empty>");
    ESP_LOGI(TAG, "  wifi_password=%s", cfg->wifi_password[0] ? "<configured>" : "<empty>");
    ESP_LOGI(TAG, "  server_base_url=%s", cfg->server_base_url[0] ? cfg->server_base_url : "<empty>");
    ESP_LOGI(TAG, "  api_token=%s", cfg->api_token[0] ? "<configured>" : "<empty>");
    ESP_LOGI(TAG, "  whisper_api_url=%s", cfg->whisper_api_url[0] ? cfg->whisper_api_url : "<empty>");
    ESP_LOGI(TAG, "  whisper_api_key=%s", cfg->whisper_api_key[0] ? "<configured>" : "<empty>");
    ESP_LOGI(TAG, "  stt_model=%s", cfg->stt_model[0] ? cfg->stt_model : "<empty>");
    ESP_LOGI(TAG, "  openai_api_base=%s", cfg->openai_api_base[0] ? cfg->openai_api_base : "<empty>");
    ESP_LOGI(TAG, "  openai_api_key=%s", cfg->openai_api_key[0] ? "<configured>" : "<empty>");
    ESP_LOGI(TAG, "  translation_model=%s", cfg->translation_model[0] ? cfg->translation_model : "<empty>");
    ESP_LOGI(TAG,
             "  translation_target_language=%s",
             cfg->translation_target_language[0] ? cfg->translation_target_language : "<empty>");
    ESP_LOGI(TAG, "  translation_enabled=%d", cfg->translation_enabled);
    ESP_LOGI(TAG, "  refine_enabled=%d", cfg->refine_enabled);
    ESP_LOGI(TAG, "  device_id=%s", cfg->device_id[0] ? cfg->device_id : "<empty>");
    ESP_LOGI(TAG,
             "  firmware_version=%s",
             cfg->firmware_version[0] ? cfg->firmware_version : "<empty>");
    ESP_LOGI(TAG, "  language=%s", cfg->language[0] ? cfg->language : "<empty>");
    ESP_LOGI(TAG, "  recording_duration_ms=%" PRIu32, cfg->recording_duration_ms);
    ESP_LOGI(TAG, "  i2s_capture_enabled=%d", VIBE_BOX_ENABLE_I2S_CAPTURE);
    ESP_LOGI(TAG, "  i2c_port=%d", VIBE_BOX_I2C_PORT);
    ESP_LOGI(TAG, "  i2c_sda_gpio=%d", VIBE_BOX_I2C_SDA_GPIO);
    ESP_LOGI(TAG, "  i2c_scl_gpio=%d", VIBE_BOX_I2C_SCL_GPIO);
    ESP_LOGI(TAG, "  codec_i2c_addr=0x%02x", VIBE_BOX_CODEC_I2C_ADDR);
    ESP_LOGI(TAG, "  pa_enable_gpio=%d", VIBE_BOX_AUDIO_PA_ENABLE_GPIO);
    ESP_LOGI(TAG, "  pa_control_gpio=%d", VIBE_BOX_AUDIO_PA_CONTROL_GPIO);
    ESP_LOGI(TAG, "  i2s_port=%d", VIBE_BOX_I2S_PORT);
    ESP_LOGI(TAG, "  i2s_mclk_gpio=%d", VIBE_BOX_I2S_MCLK_GPIO);
    ESP_LOGI(TAG, "  i2s_bclk_gpio=%d", VIBE_BOX_I2S_BCLK_GPIO);
    ESP_LOGI(TAG, "  i2s_ws_gpio=%d", VIBE_BOX_I2S_WS_GPIO);
    ESP_LOGI(TAG, "  i2s_din_gpio=%d", VIBE_BOX_I2S_DIN_GPIO);
    ESP_LOGI(TAG, "  i2s_sample_rate_hz=%d", VIBE_BOX_I2S_SAMPLE_RATE_HZ);
    ESP_LOGI(TAG, "  i2s_channels=%d", VIBE_BOX_I2S_CHANNELS);
    ESP_LOGI(TAG, "  wifi_maximum_retry=%d", CONFIG_VIBE_BOX_WIFI_MAXIMUM_RETRY);
    ESP_LOGI(TAG, "  provisioning_ap_ssid=%s", CONFIG_VIBE_BOX_PROVISIONING_AP_SSID);
}

static void log_todo_modules(void)
{
    ESP_LOGI(TAG, "pending modules: ui_epaper, touch input, sensors");
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

static void nvs_load_string_or_default(nvs_handle_t handle,
                                       const char *key,
                                       char *dst,
                                       size_t dst_size)
{
    size_t required = dst_size;

    if (nvs_get_str(handle, key, dst, &required) != ESP_OK) {
        return;
    }
}

static esp_err_t storage_load_runtime_config(runtime_config_t *cfg, bool *loaded_from_nvs)
{
    nvs_handle_t handle;
    esp_err_t err;

    runtime_config_load_defaults(cfg);
    *loaded_from_nvs = false;

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    nvs_load_string_or_default(handle, "wifi_ssid", cfg->wifi_ssid, sizeof(cfg->wifi_ssid));
    nvs_load_string_or_default(handle, "wifi_pass", cfg->wifi_password, sizeof(cfg->wifi_password));
    nvs_load_string_or_default(handle, "server_url", cfg->server_base_url, sizeof(cfg->server_base_url));
    nvs_load_string_or_default(handle, "api_token", cfg->api_token, sizeof(cfg->api_token));
    nvs_load_string_or_default(handle, "whisper_url", cfg->whisper_api_url, sizeof(cfg->whisper_api_url));
    nvs_load_string_or_default(handle, "whisper_key", cfg->whisper_api_key, sizeof(cfg->whisper_api_key));
    nvs_load_string_or_default(handle, "stt_model", cfg->stt_model, sizeof(cfg->stt_model));
    nvs_load_string_or_default(handle, "openai_base", cfg->openai_api_base, sizeof(cfg->openai_api_base));
    nvs_load_string_or_default(handle, "openai_key", cfg->openai_api_key, sizeof(cfg->openai_api_key));
    nvs_load_string_or_default(handle, "tr_model", cfg->translation_model, sizeof(cfg->translation_model));
    nvs_load_string_or_default(handle,
                               "tr_lang",
                               cfg->translation_target_language,
                               sizeof(cfg->translation_target_language));
    nvs_load_string_or_default(handle,
                               "tr_prompt",
                               cfg->translation_prompt,
                               sizeof(cfg->translation_prompt));
    nvs_load_string_or_default(handle,
                               "rf_prompt",
                               cfg->refine_prompt,
                               sizeof(cfg->refine_prompt));
    {
        uint8_t enabled = 0;

        if (nvs_get_u8(handle, "tr_en", &enabled) == ESP_OK) {
            cfg->translation_enabled = enabled != 0U;
        }
    }
    {
        uint8_t enabled = 0;

        if (nvs_get_u8(handle, "rf_en", &enabled) == ESP_OK) {
            cfg->refine_enabled = enabled != 0U;
        }
    }
    nvs_load_string_or_default(handle, "device_id", cfg->device_id, sizeof(cfg->device_id));
    nvs_load_string_or_default(handle, "fw_ver", cfg->firmware_version, sizeof(cfg->firmware_version));
    nvs_load_string_or_default(handle, "language", cfg->language, sizeof(cfg->language));
    {
        uint32_t value = 0;

        if (nvs_get_u32(handle, "rec_ms", &value) == ESP_OK && value > 0U) {
            cfg->recording_duration_ms = value;
        }
    }
    nvs_close(handle);

    *loaded_from_nvs = true;
    return ESP_OK;
}

static esp_err_t storage_save_runtime_config(const runtime_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, "wifi_ssid", cfg->wifi_ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save wifi_ssid failed: %s", esp_err_to_name(err));
        goto exit;
    }
    err = nvs_set_str(handle, "wifi_pass", cfg->wifi_password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save wifi_pass failed: %s", esp_err_to_name(err));
        goto exit;
    }
    err = nvs_set_str(handle, "server_url", cfg->server_base_url);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save server_url failed: %s", esp_err_to_name(err));
        goto exit;
    }
    err = nvs_set_str(handle, "api_token", cfg->api_token);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save api_token failed: %s", esp_err_to_name(err));
        goto exit;
    }
    err = nvs_set_str(handle, "whisper_url", cfg->whisper_api_url);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save whisper_url failed: %s", esp_err_to_name(err));
        goto exit;
    }
    err = nvs_set_str(handle, "whisper_key", cfg->whisper_api_key);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save whisper_key failed: %s", esp_err_to_name(err));
        goto exit;
    }
    err = nvs_set_str(handle, "stt_model", cfg->stt_model);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save stt_model failed: %s", esp_err_to_name(err));
        goto exit;
    }
    err = nvs_set_str(handle, "openai_base", cfg->openai_api_base);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save openai_base failed: %s", esp_err_to_name(err));
        goto exit;
    }
    err = nvs_set_str(handle, "openai_key", cfg->openai_api_key);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save openai_key failed: %s", esp_err_to_name(err));
        goto exit;
    }
    err = nvs_set_str(handle, "tr_model", cfg->translation_model);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save tr_model failed: %s", esp_err_to_name(err));
        goto exit;
    }
    err = nvs_set_str(handle, "tr_lang", cfg->translation_target_language);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save tr_lang failed: %s", esp_err_to_name(err));
        goto exit;
    }
    err = nvs_set_str(handle, "tr_prompt", cfg->translation_prompt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save tr_prompt failed: %s", esp_err_to_name(err));
        goto exit;
    }
    err = nvs_set_u8(handle, "tr_en", cfg->translation_enabled ? 1U : 0U);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save tr_en failed: %s", esp_err_to_name(err));
        goto exit;
    }
    err = nvs_set_str(handle, "rf_prompt", cfg->refine_prompt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save rf_prompt failed: %s", esp_err_to_name(err));
        goto exit;
    }
    err = nvs_set_u8(handle, "rf_en", cfg->refine_enabled ? 1U : 0U);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save rf_en failed: %s", esp_err_to_name(err));
        goto exit;
    }
    err = nvs_set_str(handle, "device_id", cfg->device_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save device_id failed: %s", esp_err_to_name(err));
        goto exit;
    }
    err = nvs_set_str(handle, "fw_ver", cfg->firmware_version);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save fw_ver failed: %s", esp_err_to_name(err));
        goto exit;
    }
    err = nvs_set_str(handle, "language", cfg->language);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save language failed: %s", esp_err_to_name(err));
        goto exit;
    }
    err = nvs_set_u32(handle, "rec_ms", cfg->recording_duration_ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save rec_ms failed: %s", esp_err_to_name(err));
        goto exit;
    }
    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commit config failed: %s", esp_err_to_name(err));
        goto exit;
    }

exit:
    nvs_close(handle);
    return err;
}

static bool json_copy_string(cJSON *root, const char *key, char *dst, size_t dst_len, bool required)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

    if (item == NULL) {
        return !required;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        strlen(item->valuestring) >= dst_len) {
        return false;
    }

    strlcpy(dst, item->valuestring, dst_len);
    return true;
}

static bool json_copy_bool(cJSON *root, const char *key, bool *dst)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

    if (item == NULL) {
        return true;
    }
    if (cJSON_IsBool(item)) {
        *dst = cJSON_IsTrue(item);
        return true;
    }
    if (cJSON_IsNumber(item)) {
        *dst = item->valuedouble != 0.0;
        return true;
    }
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        if (strcmp(item->valuestring, "true") == 0 || strcmp(item->valuestring, "1") == 0) {
            *dst = true;
            return true;
        }
        if (strcmp(item->valuestring, "false") == 0 || strcmp(item->valuestring, "0") == 0) {
            *dst = false;
            return true;
        }
    }
    return false;
}

static esp_err_t runtime_config_to_json(const runtime_config_t *cfg, char *dst, size_t dst_len)
{
    cJSON *root = NULL;
    char *printed = NULL;
    esp_err_t err = ESP_OK;

    if (cfg == NULL || dst == NULL || dst_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (cJSON_AddStringToObject(root, "wifi_ssid", cfg->wifi_ssid) == NULL ||
        cJSON_AddStringToObject(root, "wifi_password", cfg->wifi_password) == NULL ||
        cJSON_AddStringToObject(root, "whisper_api_url", cfg->whisper_api_url) == NULL ||
        cJSON_AddStringToObject(root, "whisper_api_key", cfg->whisper_api_key) == NULL ||
        cJSON_AddStringToObject(root, "stt_model", cfg->stt_model) == NULL ||
        cJSON_AddStringToObject(root, "openai_api_base", cfg->openai_api_base) == NULL ||
        cJSON_AddStringToObject(root, "openai_api_key", cfg->openai_api_key) == NULL ||
        cJSON_AddStringToObject(root, "translation_model", cfg->translation_model) == NULL ||
        cJSON_AddStringToObject(root,
                                "translation_target_language",
                                cfg->translation_target_language) == NULL ||
        cJSON_AddStringToObject(root, "translation_prompt", cfg->translation_prompt) == NULL ||
        cJSON_AddBoolToObject(root, "translation_enabled", cfg->translation_enabled) == NULL ||
        cJSON_AddStringToObject(root, "refine_prompt", cfg->refine_prompt) == NULL ||
        cJSON_AddBoolToObject(root, "refine_enabled", cfg->refine_enabled) == NULL ||
        cJSON_AddStringToObject(root, "device_id", cfg->device_id) == NULL ||
        cJSON_AddStringToObject(root, "firmware_version", cfg->firmware_version) == NULL ||
        cJSON_AddStringToObject(root, "language", cfg->language) == NULL ||
        cJSON_AddNumberToObject(root,
                                "recording_duration_ms",
                                (double)cfg->recording_duration_ms) == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    printed = cJSON_PrintUnformatted(root);
    if (printed == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    if (strlen(printed) >= dst_len) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    strlcpy(dst, printed, dst_len);

cleanup:
    if (printed != NULL) {
        cJSON_free(printed);
    }
    cJSON_Delete(root);
    return err;
}

static void fill_runtime_config_defaults(runtime_config_t *cfg)
{
    if (cfg->device_id[0] == '\0') {
        strlcpy(cfg->device_id, "vibe-box-dev", sizeof(cfg->device_id));
    }
    if (cfg->firmware_version[0] == '\0') {
        strlcpy(cfg->firmware_version, "dev", sizeof(cfg->firmware_version));
    }
    if (cfg->language[0] == '\0') {
        strlcpy(cfg->language, "zh", sizeof(cfg->language));
    }
    if (cfg->stt_model[0] == '\0') {
        strlcpy(cfg->stt_model, VIBE_BOX_DEFAULT_STT_MODEL, sizeof(cfg->stt_model));
    }
    if (cfg->translation_model[0] == '\0') {
        strlcpy(cfg->translation_model,
                VIBE_BOX_DEFAULT_TRANSLATION_MODEL,
                sizeof(cfg->translation_model));
    }
    if (cfg->translation_target_language[0] == '\0') {
        strlcpy(cfg->translation_target_language,
                VIBE_BOX_DEFAULT_TRANSLATION_TARGET_LANGUAGE,
                sizeof(cfg->translation_target_language));
    }
    if (cfg->translation_prompt[0] == '\0') {
        strlcpy(cfg->translation_prompt,
                VIBE_BOX_DEFAULT_TRANSLATION_PROMPT,
                sizeof(cfg->translation_prompt));
    }
    if (cfg->refine_prompt[0] == '\0') {
        strlcpy(cfg->refine_prompt,
                VIBE_BOX_DEFAULT_REFINE_PROMPT,
                sizeof(cfg->refine_prompt));
    }
    if (cfg->recording_duration_ms == 0U) {
        cfg->recording_duration_ms = VIBE_BOX_DEFAULT_RECORDING_DURATION_MS;
    }
}

static esp_err_t runtime_config_update_from_json(const char *json, runtime_config_t *next_config)
{
    cJSON *root = NULL;
    cJSON *rec_ms = NULL;
    esp_err_t err = ESP_OK;

    if (json == NULL || next_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!json_copy_string(root, "wifi_ssid", next_config->wifi_ssid, sizeof(next_config->wifi_ssid), true) ||
        !json_copy_string(root,
                          "wifi_password",
                          next_config->wifi_password,
                          sizeof(next_config->wifi_password),
                          false) ||
        !json_copy_string(root,
                          "whisper_api_url",
                          next_config->whisper_api_url,
                          sizeof(next_config->whisper_api_url),
                          true) ||
        !json_copy_string(root,
                          "whisper_api_key",
                          next_config->whisper_api_key,
                          sizeof(next_config->whisper_api_key),
                          false) ||
        !json_copy_string(root, "stt_model", next_config->stt_model, sizeof(next_config->stt_model), false) ||
        !json_copy_string(root,
                          "openai_api_base",
                          next_config->openai_api_base,
                          sizeof(next_config->openai_api_base),
                          false) ||
        !json_copy_string(root,
                          "openai_api_key",
                          next_config->openai_api_key,
                          sizeof(next_config->openai_api_key),
                          false) ||
        !json_copy_string(root,
                          "translation_model",
                          next_config->translation_model,
                          sizeof(next_config->translation_model),
                          false) ||
        !json_copy_string(root,
                          "translation_target_language",
                          next_config->translation_target_language,
                          sizeof(next_config->translation_target_language),
                          false) ||
        !json_copy_string(root,
                          "translation_prompt",
                          next_config->translation_prompt,
                          sizeof(next_config->translation_prompt),
                          false) ||
        !json_copy_bool(root, "translation_enabled", &next_config->translation_enabled) ||
        !json_copy_string(root,
                          "refine_prompt",
                          next_config->refine_prompt,
                          sizeof(next_config->refine_prompt),
                          false) ||
        !json_copy_bool(root, "refine_enabled", &next_config->refine_enabled) ||
        !json_copy_string(root, "device_id", next_config->device_id, sizeof(next_config->device_id), false) ||
        !json_copy_string(root,
                          "firmware_version",
                          next_config->firmware_version,
                          sizeof(next_config->firmware_version),
                          false) ||
        !json_copy_string(root, "language", next_config->language, sizeof(next_config->language), false)) {
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    rec_ms = cJSON_GetObjectItemCaseSensitive(root, "recording_duration_ms");
    if (rec_ms != NULL) {
        if (cJSON_IsNumber(rec_ms)) {
            if (rec_ms->valuedouble < 1000.0 || rec_ms->valuedouble > 15000.0) {
                err = ESP_ERR_INVALID_ARG;
                goto cleanup;
            }
            next_config->recording_duration_ms = (uint32_t)rec_ms->valuedouble;
        } else if (cJSON_IsString(rec_ms) && rec_ms->valuestring != NULL) {
            char *end = NULL;
            unsigned long parsed = strtoul(rec_ms->valuestring, &end, 10);

            if (end == NULL || *end != '\0' || parsed < 1000UL || parsed > 15000UL) {
                err = ESP_ERR_INVALID_ARG;
                goto cleanup;
            }
            next_config->recording_duration_ms = (uint32_t)parsed;
        } else {
            err = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
    }

    fill_runtime_config_defaults(next_config);
    if (!runtime_config_is_complete(next_config)) {
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

cleanup:
    cJSON_Delete(root);
    return err;
}

static esp_err_t ble_config_get_handler(char *dst, size_t dst_len, void *ctx)
{
    (void)ctx;
    return runtime_config_to_json(&s_runtime_config, dst, dst_len);
}

static esp_err_t ble_config_set_handler(const char *json, char *response, size_t response_len, void *ctx)
{
    runtime_config_t next_config = s_runtime_config;
    esp_err_t err;

    (void)ctx;
    err = runtime_config_update_from_json(json, &next_config);
    if (err != ESP_OK) {
        strlcpy(response, "{\"ok\":false,\"error\":\"invalid config\"}", response_len);
        return err;
    }

    err = storage_save_runtime_config(&next_config);
    if (err != ESP_OK) {
        snprintf(response,
                 response_len,
                 "{\"ok\":false,\"error\":\"%s\"}",
                 esp_err_to_name(err));
        return err;
    }

    s_runtime_config = next_config;
    s_provisioning_reconnect_requested = true;
    s_runtime_config_reconnect_requested = true;
    log_runtime_config(&s_runtime_config, "ble-updated");
    strlcpy(response, "{\"ok\":true,\"reconnect\":true}", response_len);
    return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_context_t *response_ctx = (http_response_context_t *)evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGW(TAG, "http event: error");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGI(TAG, "http event: connected");
        break;
    case HTTP_EVENT_HEADERS_SENT:
        ESP_LOGI(TAG, "http event: headers sent");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGI(TAG, "http header: %s=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGI(TAG, "http event: data len=%d", evt->data_len);
        if (response_ctx != NULL && response_ctx->buffer != NULL && response_ctx->capacity > 0U &&
            evt->data != NULL && evt->data_len > 0) {
            size_t available = response_ctx->capacity - response_ctx->length - 1U;
            size_t to_copy = ((size_t)evt->data_len < available) ? (size_t)evt->data_len : available;

            if (to_copy > 0U) {
                memcpy(response_ctx->buffer + response_ctx->length, evt->data, to_copy);
                response_ctx->length += to_copy;
                response_ctx->buffer[response_ctx->length] = '\0';
            }
            if (to_copy < (size_t)evt->data_len) {
                response_ctx->truncated = true;
            }
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "http event: finish");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "http event: disconnected");
        break;
    default:
        break;
    }

    return ESP_OK;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "wifi event: station start");
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event =
            (const wifi_event_sta_disconnected_t *)event_data;

        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG,
                 "wifi event: disconnected reason=%d retry=%d/%d",
                 event ? event->reason : -1,
                 s_retry_num,
                 CONFIG_VIBE_BOX_WIFI_MAXIMUM_RETRY);
        if (s_retry_num < CONFIG_VIBE_BOX_WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG,
                     "Wi-Fi disconnected, retrying (%d/%d)",
                     s_retry_num,
                     CONFIG_VIBE_BOX_WIFI_MAXIMUM_RETRY);
            return;
        }

        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        ESP_LOGE(TAG, "Wi-Fi connect failed after %d retries", CONFIG_VIBE_BOX_WIFI_MAXIMUM_RETRY);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "wifi event: provisioning ap started");
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        ESP_LOGI(TAG, "wifi event: provisioning ap stopped");
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;

        s_retry_num = 0;
        xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG,
                 "wifi event: got ip ip=" IPSTR " netmask=" IPSTR " gw=" IPSTR,
                 IP2STR(&event->ip_info.ip),
                 IP2STR(&event->ip_info.netmask),
                 IP2STR(&event->ip_info.gw));
    }
}

static esp_err_t wifi_init_once(void)
{
    esp_err_t err;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_network_stack_ready) {
        ESP_LOGI(TAG, "initializing esp_netif");
        ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");

        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }

        s_sta_netif = esp_netif_create_default_wifi_sta();
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_sta_netif == NULL || s_ap_netif == NULL) {
            return ESP_FAIL;
        }
        s_network_stack_ready = true;
    }

    if (!s_wifi_driver_ready) {
        ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");
        ESP_RETURN_ON_ERROR(
            esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL),
            TAG,
            "register wifi handler failed");
        ESP_RETURN_ON_ERROR(
            esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL),
            TAG,
            "register ip handler failed");
        s_wifi_driver_ready = true;
    }

    return ESP_OK;
}

static bool wifi_is_connected(void)
{
    if (s_wifi_event_group == NULL) {
        return false;
    }

    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

static esp_err_t wifi_stop_if_running(void)
{
    esp_err_t err;

    if (!s_wifi_driver_ready || !s_wifi_station_started) {
        return ESP_OK;
    }

    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_STOP_STATE) {
        return err;
    }

    s_wifi_station_started = false;
    return ESP_OK;
}

static esp_err_t wifi_start_station(const runtime_config_t *cfg)
{
    wifi_config_t wifi_config = {0};

    if (cfg->wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "Wi-Fi SSID not configured; staying in provisioning state");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(wifi_init_once(), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(wifi_stop_if_running(), TAG, "wifi stop failed");

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;

    strlcpy((char *)wifi_config.sta.ssid, cfg->wifi_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, cfg->wifi_password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode =
        cfg->wifi_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_LOGI(TAG, "setting wifi mode to STA");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode sta failed");
    ESP_LOGI(TAG, "applying station config ssid=%s", cfg->wifi_ssid);
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "set sta config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_MIN_MODEM), TAG, "enable wifi power save failed");
    ESP_LOGI(TAG, "starting wifi station");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start wifi sta failed");

    s_wifi_station_started = true;
    return ESP_OK;
}

static bool wait_for_wifi_connection(TickType_t timeout_ticks)
{
    EventBits_t bits;

    ESP_LOGI(TAG, "waiting for wifi connection timeout_ms=%" PRIu32, pdTICKS_TO_MS(timeout_ticks));
    bits = xEventGroupWaitBits(s_wifi_event_group,
                               WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                               pdFALSE,
                               pdFALSE,
                               timeout_ticks);

    if ((bits & WIFI_CONNECTED_BIT) != 0) {
        ESP_LOGI(TAG, "Wi-Fi connected");
        return true;
    }

    if ((bits & WIFI_FAIL_BIT) != 0) {
        ESP_LOGE(TAG, "Wi-Fi failed");
        return false;
    }

    ESP_LOGW(TAG, "Wi-Fi connect wait timed out");
    return false;
}

static esp_err_t wifi_start_provisioning_ap(void)
{
    wifi_config_t wifi_config = {0};
    esp_netif_ip_info_t ip_info = {0};
    const char *password = CONFIG_VIBE_BOX_PROVISIONING_AP_PASSWORD;

    ESP_RETURN_ON_ERROR(wifi_init_once(), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(wifi_stop_if_running(), TAG, "wifi stop failed");

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    strlcpy((char *)wifi_config.ap.ssid,
            CONFIG_VIBE_BOX_PROVISIONING_AP_SSID,
            sizeof(wifi_config.ap.ssid));
    strlcpy((char *)wifi_config.ap.password, password, sizeof(wifi_config.ap.password));
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = password[0] ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.ap.ssid_len = (uint8_t)strlen(CONFIG_VIBE_BOX_PROVISIONING_AP_SSID);

    ESP_LOGI(TAG, "setting wifi mode to AP for provisioning");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set mode ap failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), TAG, "set ap config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start wifi ap failed");

    s_wifi_station_started = true;

    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG,
                 "provisioning ap ready ssid=%s password=%s ip=" IPSTR,
                 CONFIG_VIBE_BOX_PROVISIONING_AP_SSID,
                 password[0] ? "<configured>" : "<open>",
                 IP2STR(&ip_info.ip));
    }

    return ESP_OK;
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    return -1;
}

static bool url_decode_component(const char *src, size_t src_len, char *dst, size_t dst_len)
{
    size_t i;
    size_t out = 0;

    if (dst_len == 0U) {
        return false;
    }

    for (i = 0; i < src_len; ++i) {
        char ch = src[i];

        if (ch == '+') {
            if ((out + 1U) >= dst_len) {
                return false;
            }
            dst[out++] = ' ';
            continue;
        }

        if (ch == '%' && (i + 2U) < src_len) {
            int high = hex_value(src[i + 1U]);
            int low = hex_value(src[i + 2U]);

            if (high < 0 || low < 0) {
                return false;
            }
            if ((out + 1U) >= dst_len) {
                return false;
            }
            dst[out++] = (char)((high << 4) | low);
            i += 2U;
            continue;
        }

        if ((out + 1U) >= dst_len) {
            return false;
        }
        dst[out++] = ch;
    }

    dst[out] = '\0';
    return true;
}

static bool extract_form_value(const char *body, const char *key, char *dst, size_t dst_len)
{
    const char *cursor = body;
    size_t key_len = strlen(key);

    if (dst_len == 0U) {
        return false;
    }
    dst[0] = '\0';

    while (*cursor != '\0') {
        const char *segment_end = strchr(cursor, '&');
        const char *equals;
        size_t segment_len;

        if (segment_end == NULL) {
            segment_end = cursor + strlen(cursor);
        }

        segment_len = (size_t)(segment_end - cursor);
        equals = memchr(cursor, '=', segment_len);
        if (equals != NULL && (size_t)(equals - cursor) == key_len &&
            strncmp(cursor, key, key_len) == 0) {
            return url_decode_component(equals + 1,
                                        (size_t)(segment_end - (equals + 1)),
                                        dst,
                                        dst_len);
        }

        if (*segment_end == '\0') {
            break;
        }
        cursor = segment_end + 1;
    }

    return false;
}

static bool extract_form_u32(const char *body, const char *key, uint32_t *value_out)
{
    char decoded[32];
    char *end = NULL;
    unsigned long parsed;

    if (value_out == NULL) {
        return false;
    }

    if (!extract_form_value(body, key, decoded, sizeof(decoded)) || decoded[0] == '\0') {
        return false;
    }

    parsed = strtoul(decoded, &end, 10);
    if (end == NULL || *end != '\0' || parsed == 0UL || parsed > UINT32_MAX) {
        return false;
    }

    *value_out = (uint32_t)parsed;
    return true;
}

static esp_err_t perform_http_request(const runtime_config_t *cfg,
                                      const char *path,
                                      esp_http_client_method_t method,
                                      const char *content_type,
                                      const char *bearer_token,
                                      const uint8_t *body,
                                      size_t body_len,
                                      char *response_buf,
                                      size_t response_buf_size,
                                      int *status_out)
{
    char url[URL_BUFFER_SIZE];
    http_response_context_t response_ctx = {
        .buffer = response_buf,
        .capacity = response_buf_size,
        .length = 0U,
        .truncated = false,
    };
    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .event_handler = http_event_handler,
        .timeout_ms = 120000,
        .user_data = (response_buf != NULL && response_buf_size > 0U) ? &response_ctx : NULL,
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };

    if (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0) {
        if (snprintf(url, sizeof(url), "%s", path) >= (int)sizeof(url)) {
            ESP_LOGE(TAG, "request URL too long");
            return ESP_ERR_INVALID_SIZE;
        }
    } else {
        if (snprintf(url, sizeof(url), "%s%s", cfg->server_base_url, path) >= (int)sizeof(url)) {
            ESP_LOGE(TAG, "server URL too long path=%s", path);
            return ESP_ERR_INVALID_SIZE;
        }
    }

    ESP_LOGI(TAG, "http request begin method=%d url=%s", method, url);
    if (body != NULL) {
        ESP_LOGI(TAG, "http request body_len=%u", (unsigned)body_len);
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "failed to create http client");
        return ESP_FAIL;
    }

    if (content_type != NULL) {
        ESP_ERROR_CHECK(esp_http_client_set_header(client, "Content-Type", content_type));
    }
    if (bearer_token != NULL && bearer_token[0] != '\0') {
        char bearer_header[RUNTIME_WHISPER_API_KEY_MAX + 16U];

        if (snprintf(bearer_header, sizeof(bearer_header), "Bearer %s", bearer_token) <
            (int)sizeof(bearer_header)) {
            ESP_ERROR_CHECK(esp_http_client_set_header(client, "Authorization", bearer_header));
        }
    }
    if (body != NULL && body_len > 0U) {
        ESP_ERROR_CHECK(esp_http_client_set_post_field(client, (const char *)body, (int)body_len));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (status_out != NULL) {
        *status_out = status;
    }

    if (err == ESP_OK && response_buf != NULL && response_buf_size > 0U) {
        response_buf[response_ctx.length] = '\0';
        ESP_LOGI(TAG, "http response status=%d body=%s", status, response_buf);
        if (response_ctx.truncated) {
            ESP_LOGW(TAG, "http response body truncated capacity=%u", (unsigned)response_buf_size);
        }
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "http request failed err=%s status=%d", esp_err_to_name(err), status);
    }

    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        return err;
    }

    if (status < 200 || status >= 300) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void clear_query_result(query_result_t *result)
{
    memset(result, 0, sizeof(*result));
}

static esp_err_t battery_monitor_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = VIBE_BOX_BATTERY_ADC_UNIT,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_battery_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "battery adc unit init failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = VIBE_BOX_BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_oneshot_config_channel(s_battery_adc_handle,
                                     VIBE_BOX_BATTERY_ADC_CHANNEL,
                                     &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "battery adc channel init failed: %s", esp_err_to_name(err));
        return err;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = VIBE_BOX_BATTERY_ADC_UNIT,
        .chan = VIBE_BOX_BATTERY_ADC_CHANNEL,
        .atten = VIBE_BOX_BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_battery_adc_cali_handle);
    if (err == ESP_OK) {
        s_battery_adc_calibrated = true;
    } else {
        ESP_LOGW(TAG, "battery adc calibration unavailable: %s", esp_err_to_name(err));
    }
#else
    ESP_LOGW(TAG, "battery adc calibration scheme unavailable");
#endif

    s_battery_adc_ready = true;
    ESP_LOGI(TAG, "battery monitor ready on ADC1 channel 3");
    return ESP_OK;
}

static uint8_t battery_percent_from_mv(int battery_mv)
{
    if (battery_mv <= VIBE_BOX_BATTERY_EMPTY_MV) {
        return 0;
    }
    if (battery_mv >= VIBE_BOX_BATTERY_FULL_MV) {
        return 100;
    }

    return (uint8_t)(((battery_mv - VIBE_BOX_BATTERY_EMPTY_MV) * 100 +
                      ((VIBE_BOX_BATTERY_FULL_MV - VIBE_BOX_BATTERY_EMPTY_MV) / 2)) /
                     (VIBE_BOX_BATTERY_FULL_MV - VIBE_BOX_BATTERY_EMPTY_MV));
}

static bool battery_read_percent(uint8_t *percent_out, int *battery_mv_out)
{
    if (!s_battery_adc_ready || s_battery_adc_handle == NULL || percent_out == NULL) {
        return false;
    }

    int64_t pin_mv_sum = 0;
    int sample_count = 0;

    for (int i = 0; i < VIBE_BOX_BATTERY_SAMPLE_COUNT; ++i) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(s_battery_adc_handle,
                                         VIBE_BOX_BATTERY_ADC_CHANNEL,
                                         &raw);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "battery adc read failed: %s", esp_err_to_name(err));
            continue;
        }

        int pin_mv = 0;
        if (s_battery_adc_calibrated && s_battery_adc_cali_handle != NULL) {
            err = adc_cali_raw_to_voltage(s_battery_adc_cali_handle, raw, &pin_mv);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "battery adc calibration failed: %s", esp_err_to_name(err));
                continue;
            }
        } else {
            pin_mv = (raw * 3300) / 4095;
        }

        pin_mv_sum += pin_mv;
        sample_count++;
    }

    if (sample_count == 0) {
        return false;
    }

    int battery_mv = (int)((pin_mv_sum / sample_count) * VIBE_BOX_BATTERY_DIVIDER_NUM);
    *percent_out = battery_percent_from_mv(battery_mv);
    if (battery_mv_out != NULL) {
        *battery_mv_out = battery_mv;
    }
    return true;
}

static void render_ui_status(app_state_t state, const char *headline, const char *detail)
{
    memset(&s_ui_snapshot, 0, sizeof(s_ui_snapshot));
    s_ui_snapshot.page_state = state;
    strlcpy(s_ui_snapshot.headline, headline, sizeof(s_ui_snapshot.headline));
    strlcpy(s_ui_snapshot.detail, detail, sizeof(s_ui_snapshot.detail));

    ESP_LOGI(TAG,
             "ui status page=%s headline=%s detail=%s",
             app_state_name(state),
             s_ui_snapshot.headline,
             s_ui_snapshot.detail);

    if (s_ui_dashboard_task_handle != NULL) {
        xTaskNotifyGive(s_ui_dashboard_task_handle);
    }
}

static void render_ui_query_result(const query_result_t *result)
{
    size_t i;

    memset(&s_ui_snapshot, 0, sizeof(s_ui_snapshot));
    s_ui_snapshot.page_state = APP_STATE_DISPLAYING;
    strlcpy(s_ui_snapshot.headline, "Vibe Box", sizeof(s_ui_snapshot.headline));
    strlcpy(s_ui_snapshot.detail, result->reply_text, sizeof(s_ui_snapshot.detail));
    s_ui_snapshot.line_count = result->display_line_count;

    for (i = 0; i < result->display_line_count && i < QUERY_DISPLAY_LINE_MAX; ++i) {
        strlcpy(s_ui_snapshot.lines[i], result->display_lines[i], sizeof(s_ui_snapshot.lines[i]));
        ESP_LOGI(TAG, "ui line[%u]=%s", (unsigned)i, s_ui_snapshot.lines[i]);
    }

    ESP_LOGI(TAG, "ui result detail=%s", s_ui_snapshot.detail);

    if (s_ui_dashboard_task_handle != NULL) {
        xTaskNotifyGive(s_ui_dashboard_task_handle);
    }
}

static esp_err_t parse_whisper_response(const char *response_body, query_result_t *result)
{
    cJSON *root;
    cJSON *text;

    clear_query_result(result);

    root = cJSON_Parse(response_body);
    if (root == NULL) {
        ESP_LOGE(TAG, "failed to parse Whisper JSON");
        return ESP_FAIL;
    }

    text = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (!cJSON_IsString(text) || text->valuestring == NULL) {
        text = cJSON_GetObjectItemCaseSensitive(root, "transcription");
    }
    if (!cJSON_IsString(text) || text->valuestring == NULL || text->valuestring[0] == '\0') {
        ESP_LOGE(TAG, "Whisper JSON missing text/transcription");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    strlcpy(result->transcript, text->valuestring, sizeof(result->transcript));
    strlcpy(result->reply_text, result->transcript, sizeof(result->reply_text));
    strlcpy(result->display_lines[0], result->transcript, sizeof(result->display_lines[0]));
    result->display_line_count = 1U;

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t build_openai_chat_url(const char *api_base, char *dst, size_t dst_len)
{
    const char *suffix = "chat/completions";
    size_t len;

    if (api_base == NULL || api_base[0] == '\0' || dst == NULL || dst_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strstr(api_base, "/chat/completions") != NULL) {
        if (snprintf(dst, dst_len, "%s", api_base) >= (int)dst_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        return ESP_OK;
    }

    len = strlen(api_base);
    if (snprintf(dst,
                 dst_len,
                 "%s%s%s",
                 api_base,
                 (len > 0U && api_base[len - 1U] == '/') ? "" : "/",
                 suffix) >= (int)dst_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static bool openai_api_base_is_deepseek(const char *api_base)
{
    return api_base != NULL && strstr(api_base, "deepseek.com") != NULL;
}

static esp_err_t add_deepseek_thinking_if_needed(const runtime_config_t *cfg, cJSON *root)
{
    cJSON *thinking = NULL;

    if (cfg == NULL || root == NULL || !openai_api_base_is_deepseek(cfg->openai_api_base)) {
        return ESP_OK;
    }

    thinking = cJSON_CreateObject();
    if (thinking == NULL ||
        cJSON_AddStringToObject(thinking, "type", "disabled") == NULL ||
        !cJSON_AddItemToObject(root, "thinking", thinking)) {
        cJSON_Delete(thinking);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static const char *chat_response_content(const char *response_body)
{
    cJSON *root = cJSON_Parse(response_body);
    cJSON *choices;
    cJSON *choice;
    cJSON *message;
    cJSON *content;
    const char *value = NULL;
    static char translated[QUERY_REPLY_MAX];

    translated[0] = '\0';
    if (root == NULL) {
        ESP_LOGE(TAG, "failed to parse translation JSON");
        return NULL;
    }

    choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    message = choice != NULL ? cJSON_GetObjectItemCaseSensitive(choice, "message") : NULL;
    content = message != NULL ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;
    if (!cJSON_IsString(content) || content->valuestring == NULL) {
        content = choice != NULL ? cJSON_GetObjectItemCaseSensitive(choice, "text") : NULL;
    }
    if (!cJSON_IsString(content) || content->valuestring == NULL) {
        content = cJSON_GetObjectItemCaseSensitive(root, "text");
    }

    if (cJSON_IsString(content) && content->valuestring != NULL && content->valuestring[0] != '\0') {
        strlcpy(translated, content->valuestring, sizeof(translated));
        value = translated;
    }

    cJSON_Delete(root);
    return value;
}

static esp_err_t build_translation_request_body(const runtime_config_t *cfg,
                                                const char *transcript,
                                                char **body_out)
{
    cJSON *root = NULL;
    cJSON *messages = NULL;
    cJSON *system_msg = NULL;
    cJSON *user_msg = NULL;
    char user_content[QUERY_TEXT_MAX + RUNTIME_TRANSLATION_LANGUAGE_MAX + 64];
    char *printed = NULL;
    esp_err_t err = ESP_OK;

    if (cfg == NULL || transcript == NULL || transcript[0] == '\0' || body_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    messages = cJSON_CreateArray();
    system_msg = cJSON_CreateObject();
    user_msg = cJSON_CreateObject();
    if (root == NULL || messages == NULL || system_msg == NULL || user_msg == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    snprintf(user_content,
             sizeof(user_content),
             "Target language: %s\nText:\n%s",
             cfg->translation_target_language,
             transcript);

    if (cJSON_AddStringToObject(root, "model", cfg->translation_model) == NULL ||
        cJSON_AddNumberToObject(root, "temperature", 0) == NULL ||
        cJSON_AddStringToObject(system_msg, "role", "system") == NULL ||
        cJSON_AddStringToObject(system_msg, "content", cfg->translation_prompt) == NULL ||
        cJSON_AddStringToObject(user_msg, "role", "user") == NULL ||
        cJSON_AddStringToObject(user_msg, "content", user_content) == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    err = add_deepseek_thinking_if_needed(cfg, root);
    if (err != ESP_OK) {
        goto cleanup;
    }

    if (!cJSON_AddItemToArray(messages, system_msg)) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    system_msg = NULL;
    if (!cJSON_AddItemToArray(messages, user_msg)) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    user_msg = NULL;
    if (!cJSON_AddItemToObject(root, "messages", messages)) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    messages = NULL;

    printed = cJSON_PrintUnformatted(root);
    if (printed == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    *body_out = printed;
    printed = NULL;

cleanup:
    if (printed != NULL) {
        cJSON_free(printed);
    }
    cJSON_Delete(system_msg);
    cJSON_Delete(user_msg);
    cJSON_Delete(messages);
    cJSON_Delete(root);
    return err;
}

static esp_err_t build_refine_request_body(const runtime_config_t *cfg,
                                           const char *text,
                                           char **body_out)
{
    cJSON *root = NULL;
    cJSON *messages = NULL;
    cJSON *system_msg = NULL;
    cJSON *user_msg = NULL;
    char user_content[QUERY_REPLY_MAX + 16];
    char *printed = NULL;
    esp_err_t err = ESP_OK;

    if (cfg == NULL || text == NULL || text[0] == '\0' || body_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    messages = cJSON_CreateArray();
    system_msg = cJSON_CreateObject();
    user_msg = cJSON_CreateObject();
    if (root == NULL || messages == NULL || system_msg == NULL || user_msg == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    snprintf(user_content, sizeof(user_content), "Text:\n%s", text);

    if (cJSON_AddStringToObject(root, "model", cfg->translation_model) == NULL ||
        cJSON_AddNumberToObject(root, "temperature", 0) == NULL ||
        cJSON_AddStringToObject(system_msg, "role", "system") == NULL ||
        cJSON_AddStringToObject(system_msg, "content", cfg->refine_prompt) == NULL ||
        cJSON_AddStringToObject(user_msg, "role", "user") == NULL ||
        cJSON_AddStringToObject(user_msg, "content", user_content) == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    err = add_deepseek_thinking_if_needed(cfg, root);
    if (err != ESP_OK) {
        goto cleanup;
    }

    if (!cJSON_AddItemToArray(messages, system_msg)) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    system_msg = NULL;
    if (!cJSON_AddItemToArray(messages, user_msg)) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    user_msg = NULL;
    if (!cJSON_AddItemToObject(root, "messages", messages)) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    messages = NULL;

    printed = cJSON_PrintUnformatted(root);
    if (printed == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    *body_out = printed;
    printed = NULL;

cleanup:
    if (printed != NULL) {
        cJSON_free(printed);
    }
    cJSON_Delete(system_msg);
    cJSON_Delete(user_msg);
    cJSON_Delete(messages);
    cJSON_Delete(root);
    return err;
}

static esp_err_t translate_query_result(const runtime_config_t *cfg, query_result_t *result)
{
    char url[URL_BUFFER_SIZE];
    char *request_body = NULL;
    char *response_buf = NULL;
    const char *translated;
    int status = 0;
    esp_err_t err;

    if (cfg == NULL || result == NULL || result->transcript[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->openai_api_base[0] == '\0' || cfg->translation_model[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    err = build_openai_chat_url(cfg->openai_api_base, url, sizeof(url));
    if (err != ESP_OK) {
        return err;
    }

    response_buf = calloc(1, QUERY_RESPONSE_BUFFER_SIZE);
    if (response_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = build_translation_request_body(cfg, result->transcript, &request_body);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = perform_http_request(cfg,
                               url,
                               HTTP_METHOD_POST,
                               "application/json",
                               cfg->openai_api_key,
                               (const uint8_t *)request_body,
                               strlen(request_body),
                               response_buf,
                               QUERY_RESPONSE_BUFFER_SIZE,
                               &status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "translation request failed status=%d err=%s",
                 status, esp_err_to_name(err));
        goto cleanup;
    }

    translated = chat_response_content(response_buf);
    if (translated == NULL || translated[0] == '\0') {
        err = ESP_FAIL;
        goto cleanup;
    }

    strlcpy(result->reply_text, translated, sizeof(result->reply_text));
    strlcpy(result->display_lines[0], translated, sizeof(result->display_lines[0]));
    result->display_line_count = 1U;
    ESP_LOGI(TAG, "translation result=%s", result->reply_text);

cleanup:
    if (request_body != NULL) {
        cJSON_free(request_body);
    }
    free(response_buf);
    return err;
}

static esp_err_t refine_query_result(const runtime_config_t *cfg, query_result_t *result)
{
    char url[URL_BUFFER_SIZE];
    char *request_body = NULL;
    char *response_buf = NULL;
    const char *source_text;
    const char *refined;
    int status = 0;
    esp_err_t err;

    if (cfg == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    source_text = result->reply_text[0] != '\0' ? result->reply_text : result->transcript;
    if (source_text == NULL || source_text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->openai_api_base[0] == '\0' || cfg->translation_model[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    err = build_openai_chat_url(cfg->openai_api_base, url, sizeof(url));
    if (err != ESP_OK) {
        return err;
    }

    response_buf = calloc(1, QUERY_RESPONSE_BUFFER_SIZE);
    if (response_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = build_refine_request_body(cfg, source_text, &request_body);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = perform_http_request(cfg,
                               url,
                               HTTP_METHOD_POST,
                               "application/json",
                               cfg->openai_api_key,
                               (const uint8_t *)request_body,
                               strlen(request_body),
                               response_buf,
                               QUERY_RESPONSE_BUFFER_SIZE,
                               &status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "refine request failed status=%d err=%s",
                 status, esp_err_to_name(err));
        goto cleanup;
    }

    refined = chat_response_content(response_buf);
    if (refined == NULL || refined[0] == '\0') {
        err = ESP_FAIL;
        goto cleanup;
    }

    strlcpy(result->reply_text, refined, sizeof(result->reply_text));
    strlcpy(result->display_lines[0], refined, sizeof(result->display_lines[0]));
    result->display_line_count = 1U;
    ESP_LOGI(TAG, "refine result=%s", result->reply_text);

cleanup:
    if (request_body != NULL) {
        cJSON_free(request_body);
    }
    free(response_buf);
    return err;
}

static void log_query_result(const query_result_t *result)
{
    size_t i;

    ESP_LOGI(TAG, "query result request_id=%s", result->request_id);
    ESP_LOGI(TAG, "query transcript=%s", result->transcript);
    ESP_LOGI(TAG, "query reply_text=%s", result->reply_text);
    ESP_LOGI(TAG,
             "query display_bitmap=%s (%u bytes)",
             result->has_display_bitmap ? "yes" : "no",
             (unsigned)sizeof(result->display_bitmap));
    for (i = 0; i < result->display_line_count; ++i) {
        ESP_LOGI(TAG, "query display_line[%u]=%s", (unsigned)i, result->display_lines[i]);
    }
}

static size_t append_text(char *dst, size_t dst_len, size_t offset, const char *text)
{
    int written;

    if (offset >= dst_len) {
        return 0;
    }

    written = snprintf(dst + offset, dst_len - offset, "%s", text);
    if (written < 0 || (size_t)written >= (dst_len - offset)) {
        return 0;
    }
    return offset + (size_t)written;
}

static size_t append_form_field(uint8_t *dst,
                                size_t dst_len,
                                size_t offset,
                                const char *name,
                                const char *value)
{
    int written = snprintf((char *)dst + offset,
                           dst_len - offset,
                           "--%s\r\n"
                           "Content-Disposition: form-data; name=\"%s\"\r\n\r\n"
                           "%s\r\n",
                           MULTIPART_BOUNDARY,
                           name,
                           value);

    if (written < 0 || (size_t)written >= (dst_len - offset)) {
        return 0;
    }

    return offset + (size_t)written;
}

static esp_err_t build_whisper_multipart_body(const runtime_config_t *cfg,
                                              const uint8_t *wav_bytes,
                                              size_t wav_size,
                                              uint8_t **body_out,
                                              size_t *body_len_out,
                                              char *content_type,
                                              size_t content_type_len)
{
    static const char file_header_template[] =
        "--" MULTIPART_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"recording.wav\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n";
    static const char closing_template[] = "\r\n--" MULTIPART_BOUNDARY "--\r\n";
    size_t capacity;
    size_t offset = 0;
    uint8_t *body;

    if (wav_bytes == NULL || wav_size == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (cfg == NULL || cfg->whisper_api_url[0] == '\0' || cfg->stt_model[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    capacity = wav_size + 2048U;
    body = malloc(capacity);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    offset = append_form_field(body, capacity, offset, "model", cfg->stt_model);
    offset = append_form_field(body, capacity, offset, "language", cfg->language);
    offset = append_form_field(body, capacity, offset, "response_format", "json");
    offset = append_form_field(body, capacity, offset, "temperature", "0");
    if (offset == 0U) {
        free(body);
        return ESP_ERR_INVALID_SIZE;
    }

    offset = append_text((char *)body, capacity, offset, file_header_template);
    if (offset == 0U) {
        free(body);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(body + offset, wav_bytes, wav_size);
    offset += wav_size;

    offset = append_text((char *)body, capacity, offset, closing_template);
    if (offset == 0U) {
        free(body);
        return ESP_ERR_INVALID_SIZE;
    }

    if (snprintf(content_type,
                 content_type_len,
                 "multipart/form-data; boundary=%s",
                 MULTIPART_BOUNDARY) >= (int)content_type_len) {
        free(body);
        return ESP_ERR_INVALID_SIZE;
    }

    *body_out = body;
    *body_len_out = offset;
    return ESP_OK;
}

/* Upload an already-captured WAV buffer to a Whisper-compatible STT endpoint.
 * The caller still owns wav_bytes (we only read it). */
static esp_err_t upload_wav_and_parse(const runtime_config_t *cfg,
                                      const uint8_t *wav_bytes,
                                      size_t wav_size,
                                      query_result_t *result)
{
    char content_type[128];
    char *response_buf = NULL;
    uint8_t *body = NULL;
    size_t body_len = 0;
    int status = 0;
    esp_err_t err;

    if (cfg == NULL || wav_bytes == NULL || wav_size == 0U || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    response_buf = calloc(1, QUERY_RESPONSE_BUFFER_SIZE);
    if (response_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = build_whisper_multipart_body(
        cfg, wav_bytes, wav_size, &body, &body_len, content_type, sizeof(content_type));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to build Whisper multipart body: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = perform_http_request(cfg,
                               cfg->whisper_api_url,
                               HTTP_METHOD_POST,
                               content_type,
                               cfg->whisper_api_key,
                               body,
                               body_len,
                               response_buf,
                               QUERY_RESPONSE_BUFFER_SIZE,
                               &status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Whisper upload request failed status=%d err=%s",
                 status, esp_err_to_name(err));
        goto cleanup;
    }

    err = parse_whisper_response(response_buf, result);

cleanup:
    free(body);
    free(response_buf);
    return err;
}

static esp_err_t provisioning_root_get_handler(httpd_req_t *req)
{
    char html[PROVISIONING_HTML_BUFFER_SIZE];

    snprintf(
        html,
        sizeof(html),
        "<!doctype html><html><head><meta charset='utf-8'><title>Vibe Box Setup</title>"
        "<style>body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;max-width:680px;"
        "margin:40px auto;padding:0 16px;}input,textarea{width:100%%;padding:10px;margin:8px 0 16px;"
        "box-sizing:border-box;}button{padding:12px 16px;}code{background:#f4f4f4;padding:2px 4px;}"
        "</style></head><body><h1>Vibe Box Provisioning</h1>"
        "<p>Connect the device to your Wi-Fi and Whisper-compatible STT endpoint, then submit the form. "
        "After saving, the device will leave <code>%s</code> and try to connect.</p>"
        "<form method='post' action='/configure'>"
        "<label>Wi-Fi SSID</label><input name='wifi_ssid' value='%s' maxlength='32' />"
        "<label>Wi-Fi Password</label><input name='wifi_password' type='password' value='%s' maxlength='64' />"
        "<label>Whisper API URL</label><input name='whisper_api_url' value='%s' maxlength='255' />"
        "<label>Whisper API Key</label><input name='whisper_api_key' type='password' value='%s' maxlength='255' />"
        "<label>STT Model</label><input name='stt_model' value='%s' maxlength='95' />"
        "<label>OpenAI API Base</label><input name='openai_api_base' value='%s' maxlength='255' />"
        "<label>OpenAI API Key</label><input name='openai_api_key' type='password' value='%s' maxlength='255' />"
        "<label>Translation Model</label><input name='translation_model' value='%s' maxlength='95' />"
        "<label>Translation Target Language</label><input name='translation_target_language' value='%s' maxlength='31' />"
        "<label>Translation Prompt</label><textarea name='translation_prompt' maxlength='511'>%s</textarea>"
        "<label><input name='translation_enabled' type='checkbox' value='1' %s /> Enable Translation</label>"
        "<label>Refine Prompt</label><textarea name='refine_prompt' maxlength='511'>%s</textarea>"
        "<label><input name='refine_enabled' type='checkbox' value='1' %s /> Enable Refine</label>"
        "<label>Device ID</label><input name='device_id' value='%s' maxlength='63' />"
        "<label>Firmware Version</label><input name='firmware_version' value='%s' maxlength='63' />"
        "<label>Language</label><input name='language' value='%s' maxlength='15' />"
        "<label>Recording Duration (ms)</label><input name='recording_duration_ms' type='number' min='1000' max='15000' value='%" PRIu32 "' />"
        "<button type='submit'>Save And Reconnect</button></form>"
        "<p>Provisioning AP SSID: <strong>%s</strong></p></body></html>",
        app_state_name(APP_STATE_PROVISIONING),
        s_runtime_config.wifi_ssid,
        s_runtime_config.wifi_password,
        s_runtime_config.whisper_api_url,
        s_runtime_config.whisper_api_key,
        s_runtime_config.stt_model,
        s_runtime_config.openai_api_base,
        s_runtime_config.openai_api_key,
        s_runtime_config.translation_model,
        s_runtime_config.translation_target_language,
        s_runtime_config.translation_prompt,
        s_runtime_config.translation_enabled ? "checked" : "",
        s_runtime_config.refine_prompt,
        s_runtime_config.refine_enabled ? "checked" : "",
        s_runtime_config.device_id,
        s_runtime_config.firmware_version,
        s_runtime_config.language,
        s_runtime_config.recording_duration_ms,
        CONFIG_VIBE_BOX_PROVISIONING_AP_SSID);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t provisioning_status_get_handler(httpd_req_t *req)
{
    char payload[PROVISIONING_STATUS_BUFFER_SIZE];

    snprintf(payload,
             sizeof(payload),
             "{\"state\":\"%s\",\"wifi_ssid\":\"%s\",\"whisper_api_url\":\"%s\","
             "\"whisper_api_key_configured\":%s,\"stt_model\":\"%s\","
             "\"openai_api_base\":\"%s\",\"openai_api_key_configured\":%s,"
             "\"translation_model\":\"%s\",\"translation_target_language\":\"%s\","
             "\"translation_enabled\":%s,\"refine_enabled\":%s,"
             "\"device_id\":\"%s\",\"firmware_version\":\"%s\","
             "\"language\":\"%s\",\"recording_duration_ms\":%" PRIu32 "}",
             app_state_name(APP_STATE_PROVISIONING),
             s_runtime_config.wifi_ssid,
             s_runtime_config.whisper_api_url,
             s_runtime_config.whisper_api_key[0] ? "true" : "false",
             s_runtime_config.stt_model,
             s_runtime_config.openai_api_base,
             s_runtime_config.openai_api_key[0] ? "true" : "false",
             s_runtime_config.translation_model,
             s_runtime_config.translation_target_language,
             s_runtime_config.translation_enabled ? "true" : "false",
             s_runtime_config.refine_enabled ? "true" : "false",
             s_runtime_config.device_id,
             s_runtime_config.firmware_version,
             s_runtime_config.language,
             s_runtime_config.recording_duration_ms);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t provisioning_configure_post_handler(httpd_req_t *req)
{
    char body[PROVISIONING_FORM_BUFFER_SIZE];
    runtime_config_t next_config = s_runtime_config;
    int received = 0;

    if (req->content_len <= 0 || req->content_len >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "request body too large");
        return ESP_FAIL;
    }

    while (received < req->content_len) {
        int ret = httpd_req_recv(req,
                                 body + received,
                                 (size_t)(req->content_len - received));
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to receive body");
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    ESP_LOGI(TAG, "provisioning form body=%s", body);
    extract_form_value(body, "wifi_ssid", next_config.wifi_ssid, sizeof(next_config.wifi_ssid));
    extract_form_value(body,
                       "wifi_password",
                       next_config.wifi_password,
                       sizeof(next_config.wifi_password));
    extract_form_value(body,
                       "whisper_api_url",
                       next_config.whisper_api_url,
                       sizeof(next_config.whisper_api_url));
    extract_form_value(body,
                       "whisper_api_key",
                       next_config.whisper_api_key,
                       sizeof(next_config.whisper_api_key));
    extract_form_value(body, "stt_model", next_config.stt_model, sizeof(next_config.stt_model));
    extract_form_value(body,
                       "openai_api_base",
                       next_config.openai_api_base,
                       sizeof(next_config.openai_api_base));
    extract_form_value(body,
                       "openai_api_key",
                       next_config.openai_api_key,
                       sizeof(next_config.openai_api_key));
    extract_form_value(body,
                       "translation_model",
                       next_config.translation_model,
                       sizeof(next_config.translation_model));
    extract_form_value(body,
                       "translation_target_language",
                       next_config.translation_target_language,
                       sizeof(next_config.translation_target_language));
    extract_form_value(body,
                       "translation_prompt",
                       next_config.translation_prompt,
                       sizeof(next_config.translation_prompt));
    next_config.translation_enabled = strstr(body, "translation_enabled=1") != NULL;
    extract_form_value(body,
                       "refine_prompt",
                       next_config.refine_prompt,
                       sizeof(next_config.refine_prompt));
    next_config.refine_enabled = strstr(body, "refine_enabled=1") != NULL;
    extract_form_value(body, "device_id", next_config.device_id, sizeof(next_config.device_id));
    extract_form_value(body,
                       "firmware_version",
                       next_config.firmware_version,
                       sizeof(next_config.firmware_version));
    extract_form_value(body, "language", next_config.language, sizeof(next_config.language));
    extract_form_u32(body, "recording_duration_ms", &next_config.recording_duration_ms);

    if (!runtime_config_is_complete(&next_config)) {
        httpd_resp_send_err(req,
                            HTTPD_400_BAD_REQUEST,
                            "wifi_ssid and whisper_api_url are required");
        return ESP_FAIL;
    }

    if (next_config.device_id[0] == '\0') {
        strlcpy(next_config.device_id, "vibe-box-dev", sizeof(next_config.device_id));
    }
    if (next_config.firmware_version[0] == '\0') {
        strlcpy(next_config.firmware_version, "dev", sizeof(next_config.firmware_version));
    }
    if (next_config.language[0] == '\0') {
        strlcpy(next_config.language, "zh", sizeof(next_config.language));
    }
    if (next_config.stt_model[0] == '\0') {
        strlcpy(next_config.stt_model, VIBE_BOX_DEFAULT_STT_MODEL, sizeof(next_config.stt_model));
    }
    if (next_config.translation_model[0] == '\0') {
        strlcpy(next_config.translation_model,
                VIBE_BOX_DEFAULT_TRANSLATION_MODEL,
                sizeof(next_config.translation_model));
    }
    if (next_config.translation_target_language[0] == '\0') {
        strlcpy(next_config.translation_target_language,
                VIBE_BOX_DEFAULT_TRANSLATION_TARGET_LANGUAGE,
                sizeof(next_config.translation_target_language));
    }
    if (next_config.translation_prompt[0] == '\0') {
        strlcpy(next_config.translation_prompt,
                VIBE_BOX_DEFAULT_TRANSLATION_PROMPT,
                sizeof(next_config.translation_prompt));
    }
    if (next_config.refine_prompt[0] == '\0') {
        strlcpy(next_config.refine_prompt,
                VIBE_BOX_DEFAULT_REFINE_PROMPT,
                sizeof(next_config.refine_prompt));
    }
    if (next_config.recording_duration_ms == 0U) {
        next_config.recording_duration_ms = VIBE_BOX_DEFAULT_RECORDING_DURATION_MS;
    }

    ESP_RETURN_ON_ERROR(storage_save_runtime_config(&next_config), TAG, "failed to save config");
    s_runtime_config = next_config;
    s_provisioning_reconnect_requested = true;
    log_runtime_config(&s_runtime_config, "nvs-updated");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(
        req,
        "<!doctype html><html><body><h1>Saved</h1><p>Configuration stored. "
        "The device is reconnecting now.</p></body></html>",
        HTTPD_RESP_USE_STRLEN);
}

static esp_err_t start_provisioning_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = provisioning_root_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t status = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = provisioning_status_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t configure = {
        .uri = "/configure",
        .method = HTTP_POST,
        .handler = provisioning_configure_post_handler,
        .user_ctx = NULL,
    };

    if (s_provisioning_server != NULL) {
        return ESP_OK;
    }

    config.max_uri_handlers = 8;
    config.stack_size = 8192;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    ESP_RETURN_ON_ERROR(httpd_start(&s_provisioning_server, &config), TAG, "start provisioning httpd");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_provisioning_server, &root), TAG, "register root");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_provisioning_server, &status), TAG, "register status");
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(s_provisioning_server, &configure), TAG, "register configure");
    ESP_LOGI(TAG, "provisioning web server ready on http://192.168.4.1/");
    return ESP_OK;
}

static void stop_provisioning_server(void)
{
    if (s_provisioning_server != NULL) {
        httpd_stop(s_provisioning_server);
        s_provisioning_server = NULL;
        ESP_LOGI(TAG, "provisioning web server stopped");
    }
}

static TickType_t main_loop_delay_ticks(void)
{
    return pdMS_TO_TICKS(MAIN_LOOP_IDLE_MS);
}

static esp_err_t enter_provisioning_mode(app_state_t *state, const char *reason)
{
    esp_err_t err;

    set_state(state, APP_STATE_PROVISIONING, reason);
    render_ui_status(*state, "Provisioning", "join AP and open 192.168.4.1");

    err = wifi_start_provisioning_ap();
    if (err != ESP_OK) {
        return err;
    }

    err = start_provisioning_server();
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

static esp_err_t connect_using_runtime_config(app_state_t *state, const char *reason)
{
    esp_err_t err;

    stop_provisioning_server();

    err = wifi_start_station(&s_runtime_config);
    if (err != ESP_OK) {
        return err;
    }

    if (!wait_for_wifi_connection(pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS))) {
        return ESP_FAIL;
    }

    set_state(state, APP_STATE_IDLE, reason);
    render_ui_status(*state, "Idle", "wifi connected");
    return ESP_OK;
}

static esp_err_t prepare_audio_capture_pipeline(const runtime_config_t *cfg)
{
    audio_input_i2s_config_t capture_cfg = {
        .i2s_port = VIBE_BOX_I2S_PORT,
        .i2c_port = VIBE_BOX_I2C_PORT,
        .i2c_sda_gpio = VIBE_BOX_I2C_SDA_GPIO,
        .i2c_scl_gpio = VIBE_BOX_I2C_SCL_GPIO,
        .codec_i2c_addr = VIBE_BOX_CODEC_I2C_ADDR,
        .pa_enable_gpio = VIBE_BOX_AUDIO_PA_ENABLE_GPIO,
        .pa_control_gpio = VIBE_BOX_AUDIO_PA_CONTROL_GPIO,
        .mclk_gpio = VIBE_BOX_I2S_MCLK_GPIO,
        .bclk_gpio = VIBE_BOX_I2S_BCLK_GPIO,
        .ws_gpio = VIBE_BOX_I2S_WS_GPIO,
        .din_gpio = VIBE_BOX_I2S_DIN_GPIO,
        .sample_rate_hz = VIBE_BOX_I2S_SAMPLE_RATE_HZ,
        .channels = (uint16_t)VIBE_BOX_I2S_CHANNELS,
        .bits_per_sample = 16,
        .duration_ms = cfg->recording_duration_ms,
    };

    return audio_input_prepare_i2s_capture(&capture_cfg);
}

static void ui_dashboard_render_once(void)
{
    if (!ui_epaper_is_ready()) {
        return;
    }

    if ((s_ui_snapshot.page_state == APP_STATE_IDLE ||
         s_ui_snapshot.page_state == APP_STATE_DISPLAYING) &&
        s_last_query_result.has_display_bitmap) {
        esp_err_t err = ui_epaper_show_bitmap(s_last_query_result.display_bitmap,
                                              sizeof(s_last_query_result.display_bitmap));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "epaper bitmap flush failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGD(TAG, "epaper bitmap refreshed");
        }
        return;
    }

    ui_epaper_clear();

    char line[UI_EPAPER_MAX_COLS + 1];
    int y = 4;
    const int line_step = 8;       /* 8x8 font, no extra leading */
    const int section_step = 10;   /* extra spacing after a section */
    const int x = 4;

    /* Header: Vibe Box [state] */
    snprintf(line, sizeof(line), "Vibe Box [%s]", app_state_name(s_ui_snapshot.page_state));
    ui_epaper_draw_text(x, y, line);
    y += line_step + 1;
    ui_epaper_draw_hline(y, x, UI_EPAPER_WIDTH - x - 1);
    y += 3;

    /* Wi-Fi block */
    uint8_t battery_percent = 0;
    int battery_mv = 0;
    if (battery_read_percent(&battery_percent, &battery_mv)) {
        snprintf(line, sizeof(line), "Battery: %u%%", (unsigned)battery_percent);
        ESP_LOGD(TAG, "battery=%u%% (%dmV)", (unsigned)battery_percent, battery_mv);
    } else {
        snprintf(line, sizeof(line), "Battery: --");
    }
    ui_epaper_draw_text(x, y, line);
    y += line_step;

    bool connected = wifi_is_connected();
    ui_epaper_draw_text(x, y, connected ? "WiFi: connected" : "WiFi: disconnected");
    y += line_step;

    if (connected) {
        wifi_ap_record_t ap = {0};
        const char *ssid = s_runtime_config.wifi_ssid;
        int rssi = 0;
        bool have_rssi = false;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            if (ap.ssid[0] != '\0') {
                ssid = (const char *)ap.ssid;
            }
            rssi = ap.rssi;
            have_rssi = true;
        }
        snprintf(line, sizeof(line), "SSID:%.20s", ssid);
        ui_epaper_draw_text(x, y, line);
        y += line_step;

        esp_netif_ip_info_t ip_info = {0};
        if (s_sta_netif != NULL && esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
            snprintf(line, sizeof(line), "IP: " IPSTR, IP2STR(&ip_info.ip));
        } else {
            snprintf(line, sizeof(line), "IP: -");
        }
        ui_epaper_draw_text(x, y, line);
        y += line_step;

        if (have_rssi) {
            snprintf(line, sizeof(line), "RSSI: %ddBm", rssi);
            ui_epaper_draw_text(x, y, line);
            y += line_step;
        }
    } else {
        snprintf(line, sizeof(line), "AP:%.20s", s_runtime_config.wifi_ssid);
        ui_epaper_draw_text(x, y, line);
        y += line_step;
    }

    y += section_step - line_step;
    ui_epaper_draw_hline(y, x, UI_EPAPER_WIDTH - x - 1);
    y += 3;

    /* BLE block */
    ui_epaper_draw_text(x, y, "Bluetooth:");
    y += line_step;

    snprintf(line, sizeof(line),
             "HID: %s",
             ble_keyboard_is_connected() ? "connected" : "waiting");
    ui_epaper_draw_text(x, y, line);
    y += line_step;

    const char *text_state = "waiting";
    if (ble_keyboard_text_notify_enabled()) {
        text_state = "ready";
    } else if (ble_keyboard_text_client_connected()) {
        text_state = "connected";
    }
    snprintf(line, sizeof(line), "Text: %s", text_state);
    ui_epaper_draw_text(x, y, line);
    y += line_step;

    snprintf(line, sizeof(line),
             "Config: %s",
             ble_keyboard_config_notify_enabled() ? "ready" : "waiting");
    ui_epaper_draw_text(x, y, line);
    y += line_step;

    snprintf(line,
             sizeof(line),
             "Translate: %s %.12s",
             s_runtime_config.translation_enabled ? "on" : "off",
             s_runtime_config.translation_target_language);
    ui_epaper_draw_text(x, y, line);
    y += line_step;

    snprintf(line, sizeof(line), "Refine: %s", s_runtime_config.refine_enabled ? "on" : "off");
    ui_epaper_draw_text(x, y, line);
    y += line_step;

    /* Footer: heap + uptime */
    int footer_y = UI_EPAPER_HEIGHT - line_step - 2;
    ui_epaper_draw_hline(footer_y - 3, x, UI_EPAPER_WIDTH - x - 1);
    int64_t uptime_s = esp_timer_get_time() / 1000000;
    snprintf(line, sizeof(line),
             "up:%llds heap:%luK",
             (long long)uptime_s,
             (unsigned long)(esp_get_free_heap_size() / 1024));
    ui_epaper_draw_text(x, footer_y, line);

    esp_err_t err = ui_epaper_flush();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "epaper flush failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGD(TAG, "epaper dashboard refreshed");
    }
}

static void ui_dashboard_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(UI_DASHBOARD_REFRESH_MS);
    s_ui_dashboard_task_handle = xTaskGetCurrentTaskHandle();
    while (true) {
        ui_dashboard_render_once();
        (void)ulTaskNotifyTake(pdTRUE, period);
    }
}

static esp_err_t record_button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << VIBE_BOX_RECORD_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

static esp_err_t pwr_button_init(void)
{
    if (VIBE_BOX_PWR_BUTTON_GPIO < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << VIBE_BOX_PWR_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

static void handle_pwr_double_click(void)
{
    ESP_LOGI(TAG, "PWR double click detected; reinitializing BLE");
    render_ui_status(APP_STATE_IDLE, "BLE", "resetting");

    esp_err_t err = ble_keyboard_reinitialize();
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        render_ui_status(APP_STATE_IDLE, "BLE", "waiting connection");
    } else {
        ESP_LOGW(TAG, "ble_keyboard_reinitialize failed: %s", esp_err_to_name(err));
        render_ui_status(APP_STATE_ERROR, "BLE", esp_err_to_name(err));
    }
}

/* Render "Power off" on the e-paper (best-effort) and enter deep sleep with
 * EXT1 wake-up armed on the PWR pin. Re-pressing PWR will boot the device,
 * and the early boot path requires a long-press to confirm power-on. */
static void enter_power_off(void)
{
    ESP_LOGI(TAG, "PWR long-press detected; powering off");

    if (ui_epaper_is_ready()) {
        (void)ui_epaper_show_status("Power off", "hold PWR to power on");
    }

    /* Wait for the user to release PWR before arming the wake source so we
     * don't immediately re-trigger on a still-held press. */
    if (VIBE_BOX_PWR_BUTTON_GPIO >= 0) {
        const uint32_t release_timeout_ms = 10000U;
        uint32_t waited = 0;
        while (gpio_get_level(VIBE_BOX_PWR_BUTTON_GPIO) == 0 &&
               waited < release_timeout_ms) {
            vTaskDelay(pdMS_TO_TICKS(50));
            waited += 50U;
        }
    }

    /* PWR is active-low. Use EXT1 (any-low) wake on its bit. ESP32-S3 RTC IO
     * range covers GPIO0..GPIO21 so the default GPIO18 is supported. */
    if (VIBE_BOX_PWR_BUTTON_GPIO >= 0) {
        const uint64_t mask = 1ULL << VIBE_BOX_PWR_BUTTON_GPIO;
#if SOC_PM_SUPPORT_EXT1_WAKEUP
        esp_err_t werr = esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
        if (werr != ESP_OK) {
            ESP_LOGW(TAG, "ext1 wakeup arm failed: %s", esp_err_to_name(werr));
        }
#else
        ESP_LOGW(TAG, "EXT1 wakeup not supported by SoC; device may not wake");
#endif
    }

    ESP_LOGI(TAG, "entering deep sleep");
    esp_deep_sleep_start();
}

/* If we just woke from EXT1 (PWR press), require the user to keep the button
 * held for the full long-press window before continuing the boot. If they
 * release early, re-arm wake-up and go back to deep sleep so the device only
 * actually powers on after a confirmed long-press. */
static void confirm_power_on_long_press(void)
{
    if (VIBE_BOX_PWR_BUTTON_GPIO < 0) {
        return;
    }
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause != ESP_SLEEP_WAKEUP_EXT1) {
        return;
    }

    /* Make sure we can read the pin. The PWR GPIO might still be in its
     * RTC-domain configuration from the previous deep sleep; reconfigure
     * as a normal input with pull-up. */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << VIBE_BOX_PWR_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&cfg);

    ESP_LOGI(TAG, "wake from PWR; waiting for %dms long-press to confirm power on",
             VIBE_BOX_PWR_LONG_PRESS_MS);

    const uint32_t target = (uint32_t)VIBE_BOX_PWR_LONG_PRESS_MS;
    uint32_t held_ms = 0;
    while (held_ms < target) {
        if (gpio_get_level(VIBE_BOX_PWR_BUTTON_GPIO) != 0) {
            ESP_LOGW(TAG, "PWR released after %ums; going back to sleep", (unsigned)held_ms);
#if SOC_PM_SUPPORT_EXT1_WAKEUP
            esp_sleep_enable_ext1_wakeup(1ULL << VIBE_BOX_PWR_BUTTON_GPIO,
                                         ESP_EXT1_WAKEUP_ANY_LOW);
#endif
            esp_deep_sleep_start();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        held_ms += 50U;
    }

    ESP_LOGI(TAG, "PWR long-press confirmed; powering on");
}

static const char *record_owner_name(record_owner_t owner)
{
    switch (owner) {
    case RECORD_OWNER_TOUCH:
        return "TOUCH";
    case RECORD_OWNER_NONE:
    default:
        return "none";
    }
}

static bool handle_press_and_hold_recording(record_owner_t owner)
{
    if (s_record_owner != RECORD_OWNER_NONE) {
        ESP_LOGW(TAG,
                 "%s press detected while %s recording/upload is active; ignoring",
                 record_owner_name(owner),
                 record_owner_name(s_record_owner));
        return false;
    }

    if (audio_input_recording_is_active()) {
        ESP_LOGW(TAG,
                 "%s press detected while recording already active without owner; ignoring",
                 record_owner_name(owner));
        return false;
    }

    audio_input_i2s_config_t capture_cfg = {
        .i2s_port = VIBE_BOX_I2S_PORT,
        .i2c_port = VIBE_BOX_I2C_PORT,
        .i2c_sda_gpio = VIBE_BOX_I2C_SDA_GPIO,
        .i2c_scl_gpio = VIBE_BOX_I2C_SCL_GPIO,
        .codec_i2c_addr = VIBE_BOX_CODEC_I2C_ADDR,
        .pa_enable_gpio = VIBE_BOX_AUDIO_PA_ENABLE_GPIO,
        .pa_control_gpio = VIBE_BOX_AUDIO_PA_CONTROL_GPIO,
        .mclk_gpio = VIBE_BOX_I2S_MCLK_GPIO,
        .bclk_gpio = VIBE_BOX_I2S_BCLK_GPIO,
        .ws_gpio = VIBE_BOX_I2S_WS_GPIO,
        .din_gpio = VIBE_BOX_I2S_DIN_GPIO,
        .sample_rate_hz = VIBE_BOX_I2S_SAMPLE_RATE_HZ,
        .channels = (uint16_t)VIBE_BOX_I2S_CHANNELS,
        .bits_per_sample = 16,
        .duration_ms = 0,
    };

    esp_err_t err = audio_input_recording_start(&capture_cfg, VIBE_BOX_RECORDING_MAX_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_input_recording_start failed: %s", esp_err_to_name(err));
        render_ui_status(APP_STATE_ERROR, "Recording", "mic init failed");
        return false;
    }

    s_press_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_record_owner = owner;
    render_ui_status(APP_STATE_RECORDING, "Recording", "release to send");
    return true;
}

static void handle_press_release_and_upload(record_owner_t owner)
{
    if (!audio_input_recording_is_active()) {
        return;
    }
    if (s_record_owner != owner && s_record_owner != RECORD_OWNER_NONE) {
        ESP_LOGW(TAG,
                 "%s release ignored; active recording owner is %s",
                 record_owner_name(owner),
                 record_owner_name(s_record_owner));
        return;
    }
    if (s_record_owner == RECORD_OWNER_NONE) {
        ESP_LOGW(TAG,
                 "%s release is stopping orphaned active recording",
                 record_owner_name(owner));
    }

    uint32_t pressed_ms = 0;
    if (s_press_start_ms != 0U) {
        pressed_ms = (uint32_t)(esp_timer_get_time() / 1000) - s_press_start_ms;
        s_press_start_ms = 0;
    }

    uint8_t *wav_buf = NULL;
    size_t wav_size = 0;
    uint32_t duration_ms = 0;
    esp_err_t err = audio_input_recording_stop(&wav_buf, &wav_size, &duration_ms);

    if (err == ESP_ERR_INVALID_SIZE || (err == ESP_OK && wav_size == 0U)) {
        s_record_owner = RECORD_OWNER_NONE;
        render_ui_status(APP_STATE_IDLE, "Idle", "press too short");
        ESP_LOGW(TAG, "press released after %" PRIu32 "ms with empty audio", pressed_ms);
        free(wav_buf);
        return;
    }
    if (err != ESP_OK) {
        s_record_owner = RECORD_OWNER_NONE;
        render_ui_status(APP_STATE_ERROR, "Recording", "stop failed");
        ESP_LOGE(TAG, "audio_input_recording_stop failed: %s", esp_err_to_name(err));
        free(wav_buf);
        return;
    }

    if (duration_ms < VIBE_BOX_RECORDING_MIN_MS) {
        s_record_owner = RECORD_OWNER_NONE;
        ESP_LOGW(TAG, "discarding short take: %" PRIu32 "ms", duration_ms);
        render_ui_status(APP_STATE_IDLE, "Idle", "press too short");
        free(wav_buf);
        return;
    }

    if (!wifi_is_connected()) {
        s_record_owner = RECORD_OWNER_NONE;
        render_ui_status(APP_STATE_ERROR, "Upload", "wifi disconnected");
        free(wav_buf);
        return;
    }

    char detail[64];
    snprintf(detail, sizeof(detail), "uploading %" PRIu32 " ms", duration_ms);
    render_ui_status(APP_STATE_UPLOADING, "Uploading", detail);

    err = upload_wav_and_parse(&s_runtime_config, wav_buf, wav_size, &s_last_query_result);
    free(wav_buf);

    if (err == ESP_OK) {
        if (s_runtime_config.translation_enabled) {
            char translate_detail[64];
            snprintf(translate_detail,
                     sizeof(translate_detail),
                     "to %.40s",
                     s_runtime_config.translation_target_language);
            render_ui_status(APP_STATE_UPLOADING, "Translating", translate_detail);
            esp_err_t tr_err = translate_query_result(&s_runtime_config, &s_last_query_result);
            if (tr_err != ESP_OK) {
                ESP_LOGW(TAG, "translation skipped/failed: %s", esp_err_to_name(tr_err));
            }
        }
        if (s_runtime_config.refine_enabled) {
            render_ui_status(APP_STATE_UPLOADING, "Refining", "polishing text");
            esp_err_t rf_err = refine_query_result(&s_runtime_config, &s_last_query_result);
            if (rf_err != ESP_OK) {
                ESP_LOGW(TAG, "refine skipped/failed: %s", esp_err_to_name(rf_err));
            }
        }
        log_query_result(&s_last_query_result);
        const char *text_to_input = s_last_query_result.reply_text[0] != '\0'
                                        ? s_last_query_result.reply_text
                                        : s_last_query_result.transcript;
        esp_err_t ble_err = ble_keyboard_notify_text(text_to_input);
        if (ble_err == ESP_OK) {
            ESP_LOGI(TAG, "sent transcript to BLE text input");
        } else if (ble_err == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "BLE text input client not ready");
        } else {
            ESP_LOGW(TAG, "BLE text input notify failed: %s", esp_err_to_name(ble_err));
        }
        render_ui_query_result(&s_last_query_result);
        render_ui_status(APP_STATE_IDLE, "Idle", "ready for next take");
    } else {
        render_ui_status(APP_STATE_ERROR, "Upload", esp_err_to_name(err));
    }
    s_record_owner = RECORD_OWNER_NONE;
}

static void abort_active_recording(record_owner_t owner, const char *detail)
{
    if (!audio_input_recording_is_active()) {
        return;
    }
    if (s_record_owner != owner && s_record_owner != RECORD_OWNER_NONE) {
        ESP_LOGW(TAG,
                 "%s abort ignored; active recording owner is %s",
                 record_owner_name(owner),
                 record_owner_name(s_record_owner));
        return;
    }

    esp_err_t err = audio_input_recording_stop(NULL, NULL, NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "recording abort failed: %s", esp_err_to_name(err));
    }

    s_press_start_ms = 0;
    s_record_owner = RECORD_OWNER_NONE;
    if (owner == RECORD_OWNER_TOUCH) {
        s_touch_press_start_ms = 0;
        s_touch_recording_started = false;
        s_touch_ignoring_current_press = true;
    }
    render_ui_status(APP_STATE_IDLE, "Idle", detail);
}

static bool recover_stuck_touch_recording(void)
{
    if (!audio_input_recording_is_active() || s_record_owner != RECORD_OWNER_TOUCH) {
        return false;
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t elapsed_ms = s_press_start_ms != 0U ? now_ms - s_press_start_ms : 0U;

    if (!touch_input_is_pressed()) {
        ESP_LOGW(TAG, "touch release was missed; stopping active recording");
        s_touch_press_start_ms = 0;
        s_touch_recording_started = false;
        handle_press_release_and_upload(RECORD_OWNER_TOUCH);
        return true;
    }

    if (elapsed_ms >= VIBE_BOX_RECORDING_MAX_MS + VIBE_BOX_RECORDING_TIMEOUT_GRACE_MS) {
        ESP_LOGW(TAG, "touch recording timeout after %" PRIu32 "ms; aborting", elapsed_ms);
        abort_active_recording(RECORD_OWNER_TOUCH, "recording timeout");
        return true;
    }

    return false;
}

static void toggle_translation_enabled(const char *reason)
{
    runtime_config_t next_config = s_runtime_config;

    next_config.translation_enabled = !s_runtime_config.translation_enabled;
    esp_err_t err = storage_save_runtime_config(&next_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save translation toggle failed: %s", esp_err_to_name(err));
    }

    s_runtime_config = next_config;
    ESP_LOGI(TAG,
             "translation %s via %s",
             s_runtime_config.translation_enabled ? "enabled" : "disabled",
             reason != NULL ? reason : "toggle");
    render_ui_status(APP_STATE_IDLE,
                     "Translate",
                     s_runtime_config.translation_enabled ? "enabled" : "disabled");
}

static void handle_touch_tap_enter(uint32_t release_ms, uint32_t held_ms)
{
    if (held_ms == 0U || held_ms > (uint32_t)VIBE_BOX_TOUCH_TAP_MAX_MS) {
        s_touch_last_tap_release_ms = 0;
        return;
    }

    if (s_touch_last_tap_release_ms != 0U &&
        release_ms - s_touch_last_tap_release_ms <= (uint32_t)VIBE_BOX_TOUCH_DOUBLE_TAP_MS) {
        s_touch_last_tap_release_ms = 0;
        s_touch_ignore_until_ms = release_ms + VIBE_BOX_TOUCH_POST_ENTER_IGNORE_MS;
        ESP_LOGI(TAG, "touch double tap detected; sending Enter key");

        esp_err_t err = ble_keyboard_send_key(0, BLE_HID_KEY_ENTER);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "sent Enter key over BLE HID");
        } else if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "BLE HID keyboard not connected; Enter key not sent");
        } else {
            ESP_LOGW(TAG, "BLE HID Enter key send failed: %s", esp_err_to_name(err));
        }
    } else {
        s_touch_last_tap_release_ms = release_ms;
    }
}

static void touch_hold_recording_task(void *arg)
{
    uint32_t press_start_ms = (uint32_t)(uintptr_t)arg;

    vTaskDelay(pdMS_TO_TICKS(VIBE_BOX_TOUCH_HOLD_START_MS));

    if (s_touch_press_start_ms == press_start_ms && touch_input_is_pressed() &&
        s_record_owner == RECORD_OWNER_NONE) {
        ESP_LOGI(TAG, "touch hold detected; starting recording");
        s_touch_recording_started = handle_press_and_hold_recording(RECORD_OWNER_TOUCH);
    }

    vTaskDelete(NULL);
}

static void on_touch_event(touch_event_t event, void *user_ctx)
{
    (void)user_ctx;
    if (event == TOUCH_EVENT_SWIPE_LEFT || event == TOUCH_EVENT_SWIPE_RIGHT) {
        ESP_LOGI(TAG,
                 "touch swipe %s detected; toggling translation",
                 event == TOUCH_EVENT_SWIPE_LEFT ? "left" : "right");
        s_touch_press_start_ms = 0;
        s_touch_last_tap_release_ms = 0;
        s_touch_recording_started = false;
        s_touch_ignoring_current_press = false;
        if (s_record_owner == RECORD_OWNER_TOUCH && audio_input_recording_is_active()) {
            s_touch_release_pending = false;
            s_touch_abort_pending = true;
        }
        toggle_translation_enabled(event == TOUCH_EVENT_SWIPE_LEFT ? "swipe left" : "swipe right");
        return;
    }

    if (event == TOUCH_EVENT_DOWN) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_touch_ignore_until_ms != 0U &&
            (int32_t)(now_ms - s_touch_ignore_until_ms) < 0) {
            ESP_LOGI(TAG, "touch down ignored after double tap");
            s_touch_press_start_ms = 0;
            s_touch_recording_started = false;
            s_touch_ignoring_current_press = true;
            return;
        }
        s_touch_ignore_until_ms = 0;
        s_touch_ignoring_current_press = false;
        ESP_LOGI(TAG, "touch down");
        s_touch_press_start_ms = now_ms;
        s_touch_recording_started = false;
        BaseType_t ok = xTaskCreatePinnedToCore(
            touch_hold_recording_task,
            "touch_hold",
            4096,
            (void *)(uintptr_t)s_touch_press_start_ms,
            5,
            NULL,
            tskNO_AFFINITY);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "failed to create touch hold task");
        }
    } else {
        if (s_touch_ignoring_current_press) {
            ESP_LOGI(TAG, "touch up ignored after double tap");
            s_touch_ignoring_current_press = false;
            return;
        }
        ESP_LOGI(TAG, "touch up");
        uint32_t release_ms = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t held_ms = s_touch_press_start_ms != 0U
                               ? release_ms - s_touch_press_start_ms
                               : 0U;
        bool touch_recording_started = s_touch_recording_started;
        s_touch_press_start_ms = 0;
        s_touch_recording_started = false;
        if (touch_recording_started) {
            s_touch_last_tap_release_ms = 0;
            s_touch_release_pending = true;
        } else if (s_record_owner == RECORD_OWNER_TOUCH && audio_input_recording_is_active()) {
            ESP_LOGW(TAG, "touch up arrived after recording flag cleared; stopping from main loop");
            s_touch_last_tap_release_ms = 0;
            s_touch_release_pending = true;
        } else {
            handle_touch_tap_enter(release_ms, held_ms);
        }
    }
}

static void audio_button_task(void *arg)
{
    (void)arg;

    if (record_button_init() != ESP_OK) {
        ESP_LOGE(TAG, "failed to init BOOT button GPIO%d", VIBE_BOX_RECORD_BUTTON_GPIO);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "restart trigger ready on BOOT/GPIO%d (active low)",
             VIBE_BOX_RECORD_BUTTON_GPIO);

    /* Simple debounced edge detector. The button reads 1 when released and 0
     * when held down. We require BUTTON_DEBOUNCE_SAMPLES consecutive identical
     * samples before accepting a transition. */
    int stable_level = 1;
    int candidate_level = 1;
    int candidate_count = 0;
    uint32_t press_start_ms = 0;
    bool restart_fired = false;

    while (true) {
        int level = gpio_get_level(VIBE_BOX_RECORD_BUTTON_GPIO);
        if (level == candidate_level) {
            if (candidate_count < VIBE_BOX_BUTTON_DEBOUNCE_SAMPLES) {
                candidate_count++;
            }
        } else {
            candidate_level = level;
            candidate_count = 1;
        }

        if (candidate_count >= VIBE_BOX_BUTTON_DEBOUNCE_SAMPLES &&
            candidate_level != stable_level) {
            stable_level = candidate_level;
            if (stable_level == 0) {
                ESP_LOGI(TAG, "BOOT button pressed");
                press_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
                restart_fired = false;
            } else {
                ESP_LOGI(TAG, "BOOT button released");
                press_start_ms = 0;
                restart_fired = false;
            }
        }

        if (stable_level == 0 && !restart_fired && press_start_ms != 0U) {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (now_ms - press_start_ms >= (uint32_t)VIBE_BOX_BOOT_RESTART_LONG_PRESS_MS) {
                restart_fired = true;
                ESP_LOGI(TAG, "BOOT long-press detected; restarting");
                render_ui_status(APP_STATE_IDLE, "Restarting", "BOOT long press");
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(VIBE_BOX_BUTTON_POLL_MS));
    }
}

static void pwr_button_task(void *arg)
{
    (void)arg;

    esp_err_t err = pwr_button_init();
    if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "PWR button disabled");
        vTaskDelete(NULL);
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to init PWR button GPIO%d: %s",
                 VIBE_BOX_PWR_BUTTON_GPIO, esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "PWR ready on GPIO%d (active low): double-click=BLE reset, hold %dms=power off",
             VIBE_BOX_PWR_BUTTON_GPIO, VIBE_BOX_PWR_LONG_PRESS_MS);

    int stable_level = 1;
    int candidate_level = 1;
    int candidate_count = 0;
    uint32_t last_release_ms = 0;
    uint32_t press_start_ms = 0;
    bool long_press_fired = false;

    while (true) {
        int level = gpio_get_level(VIBE_BOX_PWR_BUTTON_GPIO);
        if (level == candidate_level) {
            if (candidate_count < VIBE_BOX_BUTTON_DEBOUNCE_SAMPLES) {
                candidate_count++;
            }
        } else {
            candidate_level = level;
            candidate_count = 1;
        }

        if (candidate_count >= VIBE_BOX_BUTTON_DEBOUNCE_SAMPLES &&
            candidate_level != stable_level) {
            stable_level = candidate_level;
            if (stable_level == 0) {
                /* Press edge: start long-press timer. */
                press_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
                long_press_fired = false;
            } else {
                /* Release edge: handle double-click only if no long-press
                 * was already fired during this hold. */
                press_start_ms = 0;
                if (!long_press_fired) {
                    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
                    if (last_release_ms != 0U &&
                        now_ms - last_release_ms <= (uint32_t)VIBE_BOX_PWR_DOUBLE_CLICK_MS) {
                        last_release_ms = 0;
                        handle_pwr_double_click();
                    } else {
                        last_release_ms = now_ms;
                    }
                } else {
                    long_press_fired = false;
                }
            }
        }

        /* Continuous-hold detection: while the button is stably pressed,
         * check whether we've crossed the long-press threshold. */
        if (stable_level == 0 && !long_press_fired && press_start_ms != 0U) {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (now_ms - press_start_ms >= (uint32_t)VIBE_BOX_PWR_LONG_PRESS_MS) {
                long_press_fired = true;
                /* Cancel any pending double-click bookkeeping. */
                last_release_ms = 0;
                enter_power_off();
                /* enter_power_off() does not return on success. */
            }
        }

        vTaskDelay(pdMS_TO_TICKS(VIBE_BOX_BUTTON_POLL_MS));
    }
}

void app_main(void)
{
    app_state_t state = APP_STATE_BOOT;
    bool loaded_from_nvs = false;

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "vibe-box starting");
    ESP_LOGI(TAG, "app_main entered");
    set_state(&state, APP_STATE_BOOT, "startup");

    /* If we just woke from EXT1 (PWR press), require a confirmed long-press
     * before continuing the boot. This runs before any heavy init so a
     * spurious wake (button bounce, etc.) does not power up the device. */
    confirm_power_on_long_press();

    {
        esp_err_t epd_err = ui_epaper_init();
        if (epd_err != ESP_OK) {
            ESP_LOGE(TAG, "ui_epaper_init failed: %s", esp_err_to_name(epd_err));
        } else {
            (void)battery_monitor_init();
            ui_epaper_show_status("Vibe Box", "boot ok, init...");
            BaseType_t task_ok = xTaskCreatePinnedToCore(
                ui_dashboard_task, "ui_dash", 4096, NULL, 3, NULL, tskNO_AFFINITY);
            if (task_ok != pdPASS) {
                ESP_LOGE(TAG, "failed to create ui_dashboard_task");
            }
        }
    }

    ESP_ERROR_CHECK(storage_init());
    ESP_LOGI(TAG, "NVS ready");
    ESP_ERROR_CHECK(storage_load_runtime_config(&s_runtime_config, &loaded_from_nvs));
    log_runtime_config(&s_runtime_config, loaded_from_nvs ? "nvs+defaults" : "menuconfig-defaults");

    ble_keyboard_set_config_callbacks(ble_config_get_handler, ble_config_set_handler, NULL);
    esp_err_t ble_err = ble_keyboard_init();
    if (ble_err != ESP_OK) {
        ESP_LOGE(TAG, "ble_keyboard_init failed: %s", esp_err_to_name(ble_err));
    }

    if (VIBE_BOX_ENABLE_I2S_CAPTURE) {
        esp_err_t err = prepare_audio_capture_pipeline(&s_runtime_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "audio capture pipeline init failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "audio capture pipeline ready");
        }
    }

    if (runtime_config_is_complete(&s_runtime_config)) {
        if (connect_using_runtime_config(&state, "wifi connected") != ESP_OK) {
            set_state(&state, APP_STATE_ERROR, "initial wifi connect failed");
            render_ui_status(state, "Error", "initial wifi connect failed");
        }
    } else {
        ESP_ERROR_CHECK(enter_provisioning_mode(&state, "missing runtime config"));
    }

    log_todo_modules();
    clear_query_result(&s_last_query_result);

    if (xTaskCreatePinnedToCore(audio_button_task, "audio_btn", 6144, NULL, 5, NULL,
                                tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "failed to create audio_button_task");
    }

    if (xTaskCreatePinnedToCore(pwr_button_task, "pwr_btn", 4096, NULL, 5, NULL,
                                tskNO_AFFINITY) != pdPASS) {
        ESP_LOGE(TAG, "failed to create pwr_button_task");
    }

#if VIBE_BOX_TOUCH_ENABLE
    {
        touch_input_config_t touch_cfg = {
            .i2c_port = VIBE_BOX_I2C_PORT,
            .i2c_sda_gpio = VIBE_BOX_I2C_SDA_GPIO,
            .i2c_scl_gpio = VIBE_BOX_I2C_SCL_GPIO,
            .reset_gpio = VIBE_BOX_TOUCH_RST_GPIO,
            .int_gpio = VIBE_BOX_TOUCH_INT_GPIO,
            .i2c_addr = (uint8_t)VIBE_BOX_TOUCH_I2C_ADDR,
            .i2c_speed_hz = 100000,
            .poll_period_ms = (uint32_t)VIBE_BOX_TOUCH_POLL_MS,
            .debounce_samples = 2,
        };
        esp_err_t terr = touch_input_init(&touch_cfg, on_touch_event, NULL);
        if (terr != ESP_OK) {
            ESP_LOGE(TAG, "touch_input_init failed: %s", esp_err_to_name(terr));
        } else {
            ESP_LOGI(TAG,
                     "touch press-and-hold ready (rst=GPIO%d int=GPIO%d addr=0x%02x)",
                     VIBE_BOX_TOUCH_RST_GPIO,
                     VIBE_BOX_TOUCH_INT_GPIO,
                     VIBE_BOX_TOUCH_I2C_ADDR);
        }
    }
#endif

    while (true) {
        if (s_runtime_config_reconnect_requested && state != APP_STATE_RECORDING &&
            state != APP_STATE_UPLOADING) {
            s_runtime_config_reconnect_requested = false;
            s_provisioning_reconnect_requested = false;
            if (!runtime_config_is_complete(&s_runtime_config)) {
                ESP_ERROR_CHECK(enter_provisioning_mode(&state, "ble runtime config incomplete"));
            } else if (connect_using_runtime_config(&state, "ble runtime config applied") != ESP_OK) {
                set_state(&state, APP_STATE_ERROR, "wifi connect after BLE config failed");
                render_ui_status(state, "Error", "BLE config wifi connect failed");
            }
        }

        if (state == APP_STATE_IDLE) {
            if (!wifi_is_connected()) {
                set_state(&state, APP_STATE_ERROR, "wifi not connected");
                render_ui_status(state, "Error", "wifi disconnected");
                vTaskDelay(main_loop_delay_ticks());
                continue;
            }

            if (audio_input_recording_is_active()) {
                if (s_touch_abort_pending) {
                    s_touch_abort_pending = false;
                    s_touch_release_pending = false;
                    abort_active_recording(RECORD_OWNER_TOUCH, "swipe cancelled");
                    vTaskDelay(pdMS_TO_TICKS(200));
                    continue;
                }
                if (s_touch_release_pending) {
                    s_touch_release_pending = false;
                    handle_press_release_and_upload(RECORD_OWNER_TOUCH);
                    vTaskDelay(pdMS_TO_TICKS(200));
                    continue;
                }
                if (recover_stuck_touch_recording()) {
                    vTaskDelay(pdMS_TO_TICKS(200));
                    continue;
                }
                /* The touch handler owns the codec/I2S channel right now; leave
                 * the idle loop out of its way until release+upload finishes. */
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
        } else if (state == APP_STATE_PROVISIONING) {
            ESP_LOGI(TAG,
                     "heartbeat state=%s free_heap=%" PRIu32,
                     app_state_name(state),
                     esp_get_free_heap_size());
            ESP_LOGW(TAG,
                     "provisioning hint: connect to AP '%s' and open http://192.168.4.1/",
                     CONFIG_VIBE_BOX_PROVISIONING_AP_SSID);

            if (s_provisioning_reconnect_requested) {
                s_provisioning_reconnect_requested = false;
                if (connect_using_runtime_config(&state, "runtime config applied") != ESP_OK) {
                    set_state(&state, APP_STATE_ERROR, "wifi connect after provisioning failed");
                    render_ui_status(state, "Error", "provisioned wifi connect failed");
                }
            }
        } else {
            ESP_LOGI(TAG,
                     "heartbeat state=%s free_heap=%" PRIu32,
                     app_state_name(state),
                     esp_get_free_heap_size());

            if (state == APP_STATE_ERROR) {
                if (!runtime_config_is_complete(&s_runtime_config)) {
                    ESP_ERROR_CHECK(enter_provisioning_mode(&state, "runtime config incomplete"));
                } else if (wifi_is_connected()) {
                    set_state(&state, APP_STATE_IDLE, "automatic retry scheduled");
                    render_ui_status(state, "Idle", "automatic retry scheduled");
                } else if (connect_using_runtime_config(&state, "recovered wifi connection") !=
                           ESP_OK) {
                    ESP_LOGW(TAG,
                             "error hint: verify Wi-Fi and Whisper-compatible API URL");
                    render_ui_status(state, "Error", "waiting before reconnect retry");
                }
            }
        }

        vTaskDelay(main_loop_delay_ticks());
    }
}
