#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "vibe_box";
static EventGroupHandle_t wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define HEALTH_RESPONSE_BUFFER_SIZE 256
#define QUERY_RESPONSE_BUFFER_SIZE  2048
#define FORM_BODY_BUFFER_SIZE       1024
#define URL_BUFFER_SIZE             320
#define QUERY_DISPLAY_LINE_MAX      4
#define QUERY_DISPLAY_COL_MAX       96
#define QUERY_TEXT_MAX              256
#define QUERY_REPLY_MAX             512
#define QUERY_REQUEST_ID_MAX        64

static int s_retry_num;

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
    char request_id[QUERY_REQUEST_ID_MAX];
    char transcript[QUERY_TEXT_MAX];
    char reply_text[QUERY_REPLY_MAX];
    char display_lines[QUERY_DISPLAY_LINE_MAX][QUERY_DISPLAY_COL_MAX];
    size_t display_line_count;
} query_result_t;

static query_result_t s_last_query_result;

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

static void log_runtime_config(void)
{
    ESP_LOGI(TAG, "firmware startup diagnostics");
    ESP_LOGI(TAG, "  project=vibe_box");
    ESP_LOGI(TAG, "  free_heap=%" PRIu32, esp_get_free_heap_size());
    ESP_LOGI(TAG, "  wifi_ssid=%s", strlen(CONFIG_VIBE_BOX_WIFI_SSID) ? CONFIG_VIBE_BOX_WIFI_SSID : "<empty>");
    ESP_LOGI(TAG,
             "  wifi_password=%s",
             strlen(CONFIG_VIBE_BOX_WIFI_PASSWORD) ? "<configured>" : "<empty>");
    ESP_LOGI(TAG, "  device_id=%s", CONFIG_VIBE_BOX_DEVICE_ID);
    ESP_LOGI(TAG, "  firmware_version=%s", CONFIG_VIBE_BOX_FIRMWARE_VERSION);
    ESP_LOGI(TAG, "  server_base_url=%s", CONFIG_VIBE_BOX_SERVER_BASE_URL);
    ESP_LOGI(TAG, "  health_poll_interval_ms=%d", CONFIG_VIBE_BOX_HEALTH_POLL_INTERVAL_MS);
    ESP_LOGI(TAG, "  demo_query_enabled=%d", CONFIG_VIBE_BOX_ENABLE_DEMO_QUERY);
    ESP_LOGI(TAG, "  demo_query_text=%s", CONFIG_VIBE_BOX_DEMO_QUERY_TEXT);
    ESP_LOGI(TAG, "  query_poll_interval_ms=%d", CONFIG_VIBE_BOX_QUERY_POLL_INTERVAL_MS);
    ESP_LOGI(TAG, "  wifi_maximum_retry=%d", CONFIG_VIBE_BOX_WIFI_MAXIMUM_RETRY);
}

static void log_todo_modules(void)
{
    ESP_LOGI(TAG, "pending modules: ui_epaper, audio_input, sensors, storage");
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

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
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

        ESP_LOGW(TAG,
                 "wifi event: disconnected reason=%d retry=%d/%d",
                 event ? event->reason : -1,
                 s_retry_num,
                 CONFIG_VIBE_BOX_WIFI_MAXIMUM_RETRY);
        if (s_retry_num < CONFIG_VIBE_BOX_WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "Wi-Fi disconnected, retrying (%d/%d)",
                     s_retry_num, CONFIG_VIBE_BOX_WIFI_MAXIMUM_RETRY);
            return;
        }

        xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        ESP_LOGE(TAG, "Wi-Fi connect failed after %d retries", CONFIG_VIBE_BOX_WIFI_MAXIMUM_RETRY);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;

        s_retry_num = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG,
                 "wifi event: got ip ip=" IPSTR " netmask=" IPSTR " gw=" IPSTR,
                 IP2STR(&event->ip_info.ip),
                 IP2STR(&event->ip_info.netmask),
                 IP2STR(&event->ip_info.gw));
    }
}

static esp_err_t wifi_init_sta(void)
{
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_config = {0};

    if (strlen(CONFIG_VIBE_BOX_WIFI_SSID) == 0U) {
        ESP_LOGW(TAG, "Wi-Fi SSID not configured; staying in provisioning state");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) {
        ESP_LOGE(TAG, "failed to create wifi event group");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "initializing esp_netif");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_LOGI(TAG, "creating default event loop");
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "creating default wifi station netif");
    esp_netif_create_default_wifi_sta();
    ESP_LOGI(TAG, "initializing esp_wifi");
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    strlcpy((char *)wifi_config.sta.ssid, CONFIG_VIBE_BOX_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password,
            CONFIG_VIBE_BOX_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_LOGI(TAG, "setting wifi mode to STA");
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_LOGI(TAG, "applying wifi config");
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_LOGI(TAG, "starting wifi");
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished, connecting to SSID '%s'", CONFIG_VIBE_BOX_WIFI_SSID);
    return ESP_OK;
}

static bool wait_for_wifi_connection(TickType_t timeout_ticks)
{
    ESP_LOGI(TAG, "waiting for wifi connection timeout_ms=%" PRIu32, pdTICKS_TO_MS(timeout_ticks));
    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        timeout_ticks);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi connected");
        return true;
    }

    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Wi-Fi failed");
        return false;
    }

    ESP_LOGW(TAG, "Wi-Fi connect wait timed out");
    return false;
}

static char hex_digit(unsigned value)
{
    return (value < 10U) ? (char)('0' + value) : (char)('A' + (value - 10U));
}

static bool is_unreserved_uri_char(unsigned char ch)
{
    return isalnum((int)ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

static size_t url_encode_component(const char *src, char *dst, size_t dst_len)
{
    size_t out = 0;

    if (dst_len == 0U) {
        return 0;
    }

    while (*src != '\0') {
        unsigned char ch = (unsigned char)*src++;

        if (is_unreserved_uri_char(ch)) {
            if ((out + 1U) >= dst_len) {
                return 0;
            }
            dst[out++] = (char)ch;
            continue;
        }

        if ((out + 3U) >= dst_len) {
            return 0;
        }
        dst[out++] = '%';
        dst[out++] = hex_digit((unsigned)(ch >> 4));
        dst[out++] = hex_digit((unsigned)(ch & 0x0F));
    }

    dst[out] = '\0';
    return out;
}

static esp_err_t perform_http_request(const char *path,
                                      esp_http_client_method_t method,
                                      const char *content_type,
                                      const char *body,
                                      char *response_buf,
                                      size_t response_buf_size,
                                      int *status_out)
{
    char url[URL_BUFFER_SIZE];
    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .event_handler = http_event_handler,
        .timeout_ms = 5000,
    };

    if (snprintf(url, sizeof(url), "%s%s", CONFIG_VIBE_BOX_SERVER_BASE_URL, path) >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "server URL too long path=%s", path);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "http request begin method=%d url=%s", method, url);
    if (body != NULL) {
        ESP_LOGI(TAG, "http request body=%s", body);
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "failed to create http client");
        return ESP_FAIL;
    }

    if (content_type != NULL) {
        ESP_ERROR_CHECK(esp_http_client_set_header(client, "Content-Type", content_type));
    }
    if (body != NULL) {
        ESP_ERROR_CHECK(esp_http_client_set_post_field(client, body, (int)strlen(body)));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (status_out != NULL) {
        *status_out = status;
    }

    if (err == ESP_OK && response_buf != NULL && response_buf_size > 0U) {
        int len = esp_http_client_read_response(client, response_buf, response_buf_size - 1U);
        if (len < 0) {
            len = 0;
        }
        response_buf[len] = '\0';
        ESP_LOGI(TAG, "http response status=%d body=%s", status, response_buf);
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

static esp_err_t health_check_once(void)
{
    char response_buf[HEALTH_RESPONSE_BUFFER_SIZE] = {0};
    int status = 0;

    ESP_LOGI(TAG, "health check begin");
    return perform_http_request("/health",
                                HTTP_METHOD_GET,
                                NULL,
                                NULL,
                                response_buf,
                                sizeof(response_buf),
                                &status);
}

static void clear_query_result(query_result_t *result)
{
    memset(result, 0, sizeof(*result));
}

static void copy_json_string(cJSON *parent, const char *key, char *dst, size_t dst_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);

    if (dst_size == 0U) {
        return;
    }
    dst[0] = '\0';

    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strlcpy(dst, item->valuestring, dst_size);
    }
}

static esp_err_t parse_query_response(const char *response_body, query_result_t *result)
{
    cJSON *root;
    cJSON *display_lines;
    cJSON *line;
    size_t line_index = 0;

    clear_query_result(result);

    root = cJSON_Parse(response_body);
    if (root == NULL) {
        ESP_LOGE(TAG, "failed to parse query JSON");
        return ESP_FAIL;
    }

    copy_json_string(root, "request_id", result->request_id, sizeof(result->request_id));
    copy_json_string(root, "transcript", result->transcript, sizeof(result->transcript));
    copy_json_string(root, "reply_text", result->reply_text, sizeof(result->reply_text));

    display_lines = cJSON_GetObjectItemCaseSensitive(root, "display_lines");
    if (cJSON_IsArray(display_lines)) {
        cJSON_ArrayForEach(line, display_lines) {
            if (!cJSON_IsString(line) || line->valuestring == NULL) {
                continue;
            }
            if (line_index >= QUERY_DISPLAY_LINE_MAX) {
                break;
            }
            strlcpy(result->display_lines[line_index],
                    line->valuestring,
                    sizeof(result->display_lines[line_index]));
            line_index++;
        }
    }

    result->display_line_count = line_index;
    cJSON_Delete(root);

    if (result->reply_text[0] == '\0' && result->transcript[0] == '\0') {
        ESP_LOGE(TAG, "query JSON missing transcript and reply_text");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void log_query_result(const query_result_t *result)
{
    size_t i;

    ESP_LOGI(TAG, "query result request_id=%s", result->request_id);
    ESP_LOGI(TAG, "query transcript=%s", result->transcript);
    ESP_LOGI(TAG, "query reply_text=%s", result->reply_text);
    for (i = 0; i < result->display_line_count; ++i) {
        ESP_LOGI(TAG, "query display_line[%u]=%s", (unsigned)i, result->display_lines[i]);
    }
}

static esp_err_t query_server_once(query_result_t *result)
{
    char response_buf[QUERY_RESPONSE_BUFFER_SIZE] = {0};
    char encoded_device_id[128];
    char encoded_firmware_version[128];
    char encoded_query_text[FORM_BODY_BUFFER_SIZE];
    char form_body[FORM_BODY_BUFFER_SIZE];
    int status = 0;
    esp_err_t err;

    if (url_encode_component(CONFIG_VIBE_BOX_DEVICE_ID, encoded_device_id, sizeof(encoded_device_id)) == 0U ||
        url_encode_component(CONFIG_VIBE_BOX_FIRMWARE_VERSION,
                             encoded_firmware_version,
                             sizeof(encoded_firmware_version)) == 0U ||
        url_encode_component(CONFIG_VIBE_BOX_DEMO_QUERY_TEXT,
                             encoded_query_text,
                             sizeof(encoded_query_text)) == 0U) {
        ESP_LOGE(TAG, "failed to URL-encode form fields");
        return ESP_ERR_INVALID_SIZE;
    }

    if (snprintf(form_body,
                 sizeof(form_body),
                 "device_id=%s&firmware_version=%s&session_id=demo-session&language=zh&audio_format=text&query_text=%s",
                 encoded_device_id,
                 encoded_firmware_version,
                 encoded_query_text) >= (int)sizeof(form_body)) {
        ESP_LOGE(TAG, "query form body too large");
        return ESP_ERR_INVALID_SIZE;
    }

    err = perform_http_request("/v1/query",
                               HTTP_METHOD_POST,
                               "application/x-www-form-urlencoded",
                               form_body,
                               response_buf,
                               sizeof(response_buf),
                               &status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "query request failed status=%d err=%s", status, esp_err_to_name(err));
        return err;
    }

    return parse_query_response(response_buf, result);
}

static TickType_t main_loop_delay_ticks(void)
{
    if (CONFIG_VIBE_BOX_ENABLE_DEMO_QUERY) {
        return pdMS_TO_TICKS(CONFIG_VIBE_BOX_QUERY_POLL_INTERVAL_MS);
    }
    return pdMS_TO_TICKS(CONFIG_VIBE_BOX_HEALTH_POLL_INTERVAL_MS);
}

void app_main(void)
{
    app_state_t state = APP_STATE_BOOT;

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "vibe-box starting");
    ESP_LOGI(TAG, "app_main entered");
    log_runtime_config();
    set_state(&state, APP_STATE_BOOT, "startup");

    ESP_ERROR_CHECK(storage_init());
    ESP_LOGI(TAG, "NVS ready");

    {
        esp_err_t wifi_err = wifi_init_sta();

        if (wifi_err == ESP_ERR_INVALID_STATE) {
            set_state(&state, APP_STATE_PROVISIONING, "wifi credentials missing");
        } else {
            ESP_ERROR_CHECK(wifi_err);
            if (wait_for_wifi_connection(pdMS_TO_TICKS(30000))) {
                set_state(&state, APP_STATE_IDLE, "wifi connected");
            } else {
                set_state(&state, APP_STATE_ERROR, "wifi connect failed");
            }
        }
    }

    log_todo_modules();
    clear_query_result(&s_last_query_result);

    while (true) {
        if (state == APP_STATE_IDLE) {
            if (CONFIG_VIBE_BOX_ENABLE_DEMO_QUERY) {
                esp_err_t err;

                set_state(&state, APP_STATE_UPLOADING, "starting demo /v1/query");
                err = query_server_once(&s_last_query_result);
                if (err == ESP_OK) {
                    set_state(&state, APP_STATE_DISPLAYING, "query response stored");
                    log_query_result(&s_last_query_result);
                    set_state(&state, APP_STATE_IDLE, "ready for next demo query");
                } else {
                    set_state(&state, APP_STATE_ERROR, "demo /v1/query failed");
                }
            } else {
                esp_err_t err;

                set_state(&state, APP_STATE_UPLOADING, "starting /health probe");
                err = health_check_once();
                if (err == ESP_OK) {
                    set_state(&state, APP_STATE_IDLE, "/health probe succeeded");
                } else {
                    set_state(&state, APP_STATE_ERROR, "/health probe failed");
                }
            }
        } else {
            ESP_LOGI(TAG,
                     "heartbeat state=%s free_heap=%" PRIu32,
                     app_state_name(state),
                     esp_get_free_heap_size());

            if (state == APP_STATE_PROVISIONING) {
                ESP_LOGW(TAG, "provisioning hint: set Wi-Fi SSID/password in menuconfig");
            } else if (state == APP_STATE_ERROR) {
                ESP_LOGW(TAG, "error hint: verify Wi-Fi, server URL, and /v1/query or /health response");
                set_state(&state, APP_STATE_IDLE, "automatic retry scheduled");
            }
        }

        vTaskDelay(main_loop_delay_ticks());
    }
}
