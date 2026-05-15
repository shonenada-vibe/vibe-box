#include "ble_keyboard.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"
#include "esp_gatts_api.h"
#include "esp_hid_common.h"
#include "esp_hidd.h"
#include "esp_hidd_gatts.h"
#include "esp_log.h"
#include "sdkconfig.h"

#define VIBE_BOX_BLE_NAME "VibeBox"
#define HID_REPORT_ID_KEYBOARD 1
#define TEXT_GATTS_APP_ID 0x00A5
#define TEXT_SERVICE_INST_ID 0
#define TEXT_NOTIFY_MTU_PAYLOAD_MAX 20
#define TEXT_NOTIFY_CHUNK_MAX (TEXT_NOTIFY_MTU_PAYLOAD_MAX - 1)
#define TEXT_OPCODE_BEGIN 0x01
#define TEXT_OPCODE_CHUNK 0x02
#define TEXT_OPCODE_END 0x03
#define CONFIG_BUFFER_SIZE 8192
#define CONFIG_OPCODE_GET 0x10
#define CONFIG_OPCODE_SAVE_BEGIN 0x21
#define CONFIG_OPCODE_SAVE_CHUNK 0x22
#define CONFIG_OPCODE_SAVE_END 0x23
#define CONFIG_OPCODE_RESP_BEGIN 0x81
#define CONFIG_OPCODE_RESP_CHUNK 0x82
#define CONFIG_OPCODE_RESP_END 0x83
#define CONFIG_OPCODE_RESP_ERROR 0x84
#define GATT_CCC_NOTIFY_ENABLE 0x0001
#define CONFIG_NOTIFY_TASK_STACK 4096
#define CONFIG_NOTIFY_TASK_PRIORITY 5
#define CONFIG_REQUEST_TASK_STACK 8192
#define CONFIG_REQUEST_TASK_PRIORITY 5

static const char *TAG = "ble_keyboard";

/* 48f2d101-7a15-4b3f-8d67-60587f5d1001 */
static const uint8_t text_service_uuid[ESP_UUID_LEN_128] = {
    0x01, 0x10, 0x5d, 0x7f, 0x58, 0x60, 0x67, 0x8d,
    0x3f, 0x4b, 0x15, 0x7a, 0x01, 0xd1, 0xf2, 0x48,
};

/* 48f2d101-7a15-4b3f-8d67-60587f5d1002 */
static const uint8_t text_char_uuid[ESP_UUID_LEN_128] = {
    0x02, 0x10, 0x5d, 0x7f, 0x58, 0x60, 0x67, 0x8d,
    0x3f, 0x4b, 0x15, 0x7a, 0x01, 0xd1, 0xf2, 0x48,
};

/* 48f2d101-7a15-4b3f-8d67-60587f5d1003 */
static const uint8_t config_char_uuid[ESP_UUID_LEN_128] = {
    0x03, 0x10, 0x5d, 0x7f, 0x58, 0x60, 0x67, 0x8d,
    0x3f, 0x4b, 0x15, 0x7a, 0x01, 0xd1, 0xf2, 0x48,
};

static const uint8_t hid_service_uuid128[ESP_UUID_LEN_128] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
};

static const uint16_t primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t char_declare_uuid = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t client_config_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static const uint8_t char_prop_notify_read_write =
    ESP_GATT_CHAR_PROP_BIT_NOTIFY | ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE;
static uint8_t text_value[TEXT_NOTIFY_MTU_PAYLOAD_MAX] = {0};
static uint8_t config_value[TEXT_NOTIFY_MTU_PAYLOAD_MAX] = {0};
static uint8_t text_ccc[2] = {0, 0};
static uint8_t config_ccc[2] = {0, 0};

enum {
    TEXT_IDX_SVC,
    TEXT_IDX_CHAR_DECL,
    TEXT_IDX_CHAR_VAL,
    TEXT_IDX_CHAR_CCC,
    TEXT_IDX_CONFIG_CHAR_DECL,
    TEXT_IDX_CONFIG_CHAR_VAL,
    TEXT_IDX_CONFIG_CHAR_CCC,
    TEXT_IDX_NB,
};

static const esp_gatts_attr_db_t text_gatt_db[TEXT_IDX_NB] = {
    [TEXT_IDX_SVC] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid, ESP_GATT_PERM_READ,
          ESP_UUID_LEN_128, ESP_UUID_LEN_128, (uint8_t *)text_service_uuid}},

    [TEXT_IDX_CHAR_DECL] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_16, (uint8_t *)&char_declare_uuid, ESP_GATT_PERM_READ,
          sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&char_prop_notify_read_write}},

    [TEXT_IDX_CHAR_VAL] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_128, (uint8_t *)text_char_uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
          sizeof(text_value), 0, text_value}},

    [TEXT_IDX_CHAR_CCC] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_16, (uint8_t *)&client_config_uuid,
          ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, sizeof(text_ccc), sizeof(text_ccc), text_ccc}},

    [TEXT_IDX_CONFIG_CHAR_DECL] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_16, (uint8_t *)&char_declare_uuid, ESP_GATT_PERM_READ,
          sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&char_prop_notify_read_write}},

    [TEXT_IDX_CONFIG_CHAR_VAL] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_128, (uint8_t *)config_char_uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
          sizeof(config_value), 0, config_value}},

    [TEXT_IDX_CONFIG_CHAR_CCC] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_16, (uint8_t *)&client_config_uuid,
          ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, sizeof(config_ccc), sizeof(config_ccc), config_ccc}},
};

static const uint8_t keyboard_report_map[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x03, 0x95, 0x05,
    0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
    0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x03,
    0x95, 0x05, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0,
};

static esp_hid_raw_report_map_t report_maps[] = {
    {
        .data = keyboard_report_map,
        .len = sizeof(keyboard_report_map),
    },
};

static esp_hid_device_config_t hid_config = {
    .vendor_id = 0x16C0,
    .product_id = 0x05DF,
    .version = 0x0100,
    .device_name = VIBE_BOX_BLE_NAME,
    .manufacturer_name = "VibeBox",
    .serial_number = "vibebox-esp32s3",
    .report_maps = report_maps,
    .report_maps_len = 1,
};

static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = ESP_HID_APPEARANCE_KEYBOARD,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(hid_service_uuid128),
    .p_service_uuid = (uint8_t *)hid_service_uuid128,
    .flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT,
};

static esp_ble_adv_params_t adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x30,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_RANDOM,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static esp_hidd_dev_t *s_hid_dev;
static uint16_t s_text_handles[TEXT_IDX_NB];
static esp_gatt_if_t s_text_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_text_conn_id;
static esp_bd_addr_t s_text_remote_bda;
static esp_bd_addr_t s_peer_remote_bda;
static esp_bd_addr_t s_random_addr;
static ble_keyboard_config_get_cb_t s_config_get_cb;
static ble_keyboard_config_set_cb_t s_config_set_cb;
static void *s_config_ctx;
static char s_config_rx_buffer[CONFIG_BUFFER_SIZE];
static size_t s_config_rx_len;
static bool s_peer_connected;
static bool s_text_connected;
static bool s_text_notify_enabled;
static bool s_config_notify_enabled;
static bool s_started;
static bool s_adv_data_ready;
static bool s_random_addr_ready;
static TaskHandle_t s_config_notify_task_handle;
static TaskHandle_t s_config_request_task_handle;

typedef struct {
    uint8_t end_opcode;
    char payload[];
} config_notify_job_t;

typedef struct {
    uint8_t opcode;
    char payload[];
} config_request_job_t;

void ble_keyboard_set_config_callbacks(ble_keyboard_config_get_cb_t get_cb,
                                       ble_keyboard_config_set_cb_t set_cb,
                                       void *ctx)
{
    s_config_get_cb = get_cb;
    s_config_set_cb = set_cb;
    s_config_ctx = ctx;
}

static esp_err_t start_advertising(void)
{
    if (!s_adv_data_ready || !s_random_addr_ready) {
        ESP_LOGD(TAG, "advertising delayed adv=%d rand=%d", s_adv_data_ready, s_random_addr_ready);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_ble_gap_start_advertising(&adv_params);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "start advertising failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        s_adv_data_ready = true;
        start_advertising();
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "advertising start failed: 0x%x", param->adv_start_cmpl.status);
        } else {
            ESP_LOGI(TAG, "advertising as %s", VIBE_BOX_BLE_NAME);
        }
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        ESP_LOGI(TAG, "pairing %s", param->ble_security.auth_cmpl.success ? "complete" : "failed");
        break;
    case ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT:
        if (param->set_rand_addr_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_random_addr_ready = true;
            ESP_LOGI(TAG, "random BLE address set: " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(s_random_addr));
            start_advertising();
        } else {
            ESP_LOGW(TAG, "set random BLE address failed: 0x%x", param->set_rand_addr_cmpl.status);
        }
        break;
    default:
        break;
    }
}

static esp_err_t set_new_random_address(void)
{
    s_random_addr_ready = false;

    ESP_RETURN_ON_ERROR(esp_ble_gap_addr_create_static(s_random_addr), TAG, "create random address");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_rand_addr(s_random_addr), TAG, "set random address");
    return ESP_OK;
}

static esp_err_t notify_packet(uint16_t handle, uint8_t opcode, const uint8_t *payload, size_t payload_len)
{
    if (!s_text_connected || s_text_gatts_if == ESP_GATT_IF_NONE || handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (payload_len > TEXT_NOTIFY_CHUNK_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t packet[TEXT_NOTIFY_MTU_PAYLOAD_MAX] = {opcode};
    if (payload_len > 0) {
        memcpy(&packet[1], payload, payload_len);
    }

    return esp_ble_gatts_send_indicate(s_text_gatts_if, s_text_conn_id,
                                       handle, payload_len + 1, packet, false);
}

static esp_err_t config_notify_response(uint8_t end_opcode, const char *payload)
{
    esp_err_t err;
    const uint8_t *cursor = (const uint8_t *)(payload != NULL ? payload : "");
    size_t len = strlen((const char *)cursor);

    if (!s_config_notify_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    err = notify_packet(s_text_handles[TEXT_IDX_CONFIG_CHAR_VAL], CONFIG_OPCODE_RESP_BEGIN, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }

    while (len > 0) {
        size_t chunk_len = len > TEXT_NOTIFY_CHUNK_MAX ? TEXT_NOTIFY_CHUNK_MAX : len;

        err = notify_packet(s_text_handles[TEXT_IDX_CONFIG_CHAR_VAL],
                            CONFIG_OPCODE_RESP_CHUNK,
                            cursor,
                            chunk_len);
        if (err != ESP_OK) {
            return err;
        }
        cursor += chunk_len;
        len -= chunk_len;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return notify_packet(s_text_handles[TEXT_IDX_CONFIG_CHAR_VAL], end_opcode, NULL, 0);
}

static void config_notify_task(void *arg)
{
    config_notify_job_t *job = (config_notify_job_t *)arg;
    esp_err_t err = config_notify_response(job->end_opcode, job->payload);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config notify response failed: %s", esp_err_to_name(err));
    }

    free(job);
    s_config_notify_task_handle = NULL;
    vTaskDelete(NULL);
}

static esp_err_t config_queue_response(uint8_t end_opcode, const char *payload)
{
    const char *text = payload != NULL ? payload : "";
    size_t payload_len = strlen(text);
    config_notify_job_t *job;

    if (!s_config_notify_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_config_request_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_config_notify_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    job = malloc(sizeof(*job) + payload_len + 1U);
    if (job == NULL) {
        return ESP_ERR_NO_MEM;
    }

    job->end_opcode = end_opcode;
    memcpy(job->payload, text, payload_len + 1U);

    BaseType_t ok = xTaskCreate(
        config_notify_task,
        "cfg_notify",
        CONFIG_NOTIFY_TASK_STACK,
        job,
        CONFIG_NOTIFY_TASK_PRIORITY,
        &s_config_notify_task_handle);
    if (ok != pdPASS) {
        s_config_notify_task_handle = NULL;
        free(job);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void config_send_error(const char *message)
{
    esp_err_t err = config_queue_response(CONFIG_OPCODE_RESP_ERROR,
                                          message != NULL ? message : "config request failed");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config error queue failed: %s", esp_err_to_name(err));
    }
}

static void config_request_task(void *arg)
{
    config_request_job_t *job = (config_request_job_t *)arg;
    esp_err_t err;

    if (job->opcode == CONFIG_OPCODE_GET) {
        char *response = calloc(1, CONFIG_BUFFER_SIZE);
        if (response == NULL) {
            (void)config_notify_response(CONFIG_OPCODE_RESP_ERROR, "config get out of memory");
            goto cleanup;
        }
        if (s_config_get_cb == NULL) {
            (void)config_notify_response(CONFIG_OPCODE_RESP_ERROR, "config get callback missing");
            free(response);
            goto cleanup;
        }

        err = s_config_get_cb(response, CONFIG_BUFFER_SIZE, s_config_ctx);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "config get failed: %s", esp_err_to_name(err));
            (void)config_notify_response(CONFIG_OPCODE_RESP_ERROR, esp_err_to_name(err));
            free(response);
            goto cleanup;
        }

        err = config_notify_response(CONFIG_OPCODE_RESP_END, response);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "config get response notify failed: %s", esp_err_to_name(err));
        }
        free(response);
    } else if (job->opcode == CONFIG_OPCODE_SAVE_END) {
        char response[256] = {0};

        if (s_config_set_cb == NULL) {
            (void)config_notify_response(CONFIG_OPCODE_RESP_ERROR, "config set callback missing");
            goto cleanup;
        }

        err = s_config_set_cb(job->payload, response, sizeof(response), s_config_ctx);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "config set failed: %s", esp_err_to_name(err));
            (void)config_notify_response(CONFIG_OPCODE_RESP_ERROR,
                                         response[0] != '\0' ? response : esp_err_to_name(err));
            goto cleanup;
        }

        err = config_notify_response(CONFIG_OPCODE_RESP_END,
                                     response[0] != '\0' ? response : "{\"ok\":true}");
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "config set response notify failed: %s", esp_err_to_name(err));
        }
    }

cleanup:
    free(job);
    s_config_request_task_handle = NULL;
    vTaskDelete(NULL);
}

static esp_err_t config_queue_request(uint8_t opcode, const char *payload)
{
    const char *text = payload != NULL ? payload : "";
    size_t payload_len = strlen(text);
    config_request_job_t *job;

    if (!s_config_notify_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_config_request_task_handle != NULL || s_config_notify_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    job = malloc(sizeof(*job) + payload_len + 1U);
    if (job == NULL) {
        return ESP_ERR_NO_MEM;
    }

    job->opcode = opcode;
    memcpy(job->payload, text, payload_len + 1U);

    BaseType_t ok = xTaskCreate(
        config_request_task,
        "cfg_request",
        CONFIG_REQUEST_TASK_STACK,
        job,
        CONFIG_REQUEST_TASK_PRIORITY,
        &s_config_request_task_handle);
    if (ok != pdPASS) {
        s_config_request_task_handle = NULL;
        free(job);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void config_handle_get(void)
{
    esp_err_t err = config_queue_request(CONFIG_OPCODE_GET, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config get request queue failed: %s", esp_err_to_name(err));
    }
}

static void config_handle_save_end(void)
{
    esp_err_t err;

    if (s_config_rx_len >= sizeof(s_config_rx_buffer)) {
        s_config_rx_len = 0;
        config_send_error("config payload too large");
        return;
    }

    s_config_rx_buffer[s_config_rx_len] = '\0';
    err = config_queue_request(CONFIG_OPCODE_SAVE_END, s_config_rx_buffer);
    s_config_rx_len = 0;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config set request queue failed: %s", esp_err_to_name(err));
    }
}

static void config_handle_write(const uint8_t *value, uint16_t len)
{
    uint8_t opcode;
    const uint8_t *payload;
    size_t payload_len;

    if (value == NULL || len == 0) {
        return;
    }

    opcode = value[0];
    payload = &value[1];
    payload_len = len - 1U;

    switch (opcode) {
    case CONFIG_OPCODE_GET:
        config_handle_get();
        break;
    case CONFIG_OPCODE_SAVE_BEGIN:
        s_config_rx_len = 0;
        break;
    case CONFIG_OPCODE_SAVE_CHUNK:
        if ((s_config_rx_len + payload_len) >= sizeof(s_config_rx_buffer)) {
            s_config_rx_len = sizeof(s_config_rx_buffer);
            config_send_error("config payload too large");
            return;
        }
        memcpy(&s_config_rx_buffer[s_config_rx_len], payload, payload_len);
        s_config_rx_len += payload_len;
        break;
    case CONFIG_OPCODE_SAVE_END:
        config_handle_save_end();
        break;
    default:
        config_send_error("unknown config opcode");
        break;
    }
}

static void text_gatts_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                               esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        if (param->reg.status != ESP_GATT_OK || param->reg.app_id != TEXT_GATTS_APP_ID) {
            return;
        }
        s_text_gatts_if = gatts_if;
        ESP_ERROR_CHECK(esp_ble_gatts_create_attr_tab(text_gatt_db, gatts_if, TEXT_IDX_NB, TEXT_SERVICE_INST_ID));
        break;
    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status != ESP_GATT_OK || param->add_attr_tab.num_handle != TEXT_IDX_NB) {
            ESP_LOGE(TAG, "create text attr table failed: status=%d handles=%d",
                     param->add_attr_tab.status, param->add_attr_tab.num_handle);
            return;
        }
        memcpy(s_text_handles, param->add_attr_tab.handles, sizeof(s_text_handles));
        ESP_ERROR_CHECK(esp_ble_gatts_start_service(s_text_handles[TEXT_IDX_SVC]));
        ESP_LOGI(TAG, "text notify service ready svc=0x%04x val=0x%04x ccc=0x%04x",
                 s_text_handles[TEXT_IDX_SVC],
                 s_text_handles[TEXT_IDX_CHAR_VAL],
                 s_text_handles[TEXT_IDX_CHAR_CCC]);
        ESP_LOGI(TAG, "config service ready val=0x%04x ccc=0x%04x",
                 s_text_handles[TEXT_IDX_CONFIG_CHAR_VAL],
                 s_text_handles[TEXT_IDX_CONFIG_CHAR_CCC]);
        break;
    case ESP_GATTS_CONNECT_EVT:
        s_text_connected = true;
        s_text_conn_id = param->connect.conn_id;
        memcpy(s_text_remote_bda, param->connect.remote_bda, sizeof(s_text_remote_bda));
        ESP_LOGI(TAG, "text client connected");
        break;
    case ESP_GATTS_DISCONNECT_EVT:
        s_text_connected = false;
        s_text_notify_enabled = false;
        s_config_notify_enabled = false;
        s_config_rx_len = 0;
        memset(s_text_remote_bda, 0, sizeof(s_text_remote_bda));
        ESP_LOGI(TAG, "text client disconnected");
        start_advertising();
        break;
    case ESP_GATTS_WRITE_EVT:
        if ((param->write.handle == s_text_handles[TEXT_IDX_CHAR_CCC] ||
             param->write.handle == s_text_handles[TEXT_IDX_CHAR_VAL]) &&
            param->write.len == sizeof(text_ccc)) {
            uint16_t ccc_value = (uint16_t)param->write.value[0] | ((uint16_t)param->write.value[1] << 8);
            s_text_notify_enabled = (ccc_value & GATT_CCC_NOTIFY_ENABLE) != 0;
            ESP_LOGI(TAG, "text notifications %s via handle 0x%04x",
                     s_text_notify_enabled ? "enabled" : "disabled",
                     param->write.handle);
        } else if (param->write.handle == s_text_handles[TEXT_IDX_CONFIG_CHAR_CCC] &&
                   param->write.len == sizeof(config_ccc)) {
            uint16_t ccc_value = (uint16_t)param->write.value[0] | ((uint16_t)param->write.value[1] << 8);
            s_config_notify_enabled = (ccc_value & GATT_CCC_NOTIFY_ENABLE) != 0;
            ESP_LOGI(TAG, "config notifications %s", s_config_notify_enabled ? "enabled" : "disabled");
        } else if (param->write.handle == s_text_handles[TEXT_IDX_CONFIG_CHAR_VAL]) {
            config_handle_write(param->write.value, param->write.len);
        }
        break;
    default:
        break;
    }
}

static void gatts_dispatcher(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                             esp_ble_gatts_cb_param_t *param)
{
    bool text_event = false;

    if (event == ESP_GATTS_CONNECT_EVT) {
        s_peer_connected = true;
        memcpy(s_peer_remote_bda, param->connect.remote_bda, sizeof(s_peer_remote_bda));
    } else if (event == ESP_GATTS_DISCONNECT_EVT) {
        s_peer_connected = false;
        memset(s_peer_remote_bda, 0, sizeof(s_peer_remote_bda));
    }

    if (event == ESP_GATTS_REG_EVT && param->reg.app_id == TEXT_GATTS_APP_ID) {
        text_event = true;
    } else if (s_text_gatts_if != ESP_GATT_IF_NONE && gatts_if == s_text_gatts_if) {
        text_event = true;
    }

    if (text_event) {
        text_gatts_handler(event, gatts_if, param);
        return;
    }

    esp_hidd_gatts_event_handler(event, gatts_if, param);
}

static void hidd_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "HID started");
        break;
    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "HID keyboard connected");
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        ESP_LOGI(TAG, "HID keyboard disconnected: %s",
                 esp_hid_disconnect_reason_str(esp_hidd_dev_transport_get(param->disconnect.dev),
                                               param->disconnect.reason));
        start_advertising();
        break;
    case ESP_HIDD_OUTPUT_EVENT:
        ESP_LOGD(TAG, "HID output report id=%u len=%u", param->output.report_id, param->output.length);
        break;
    default:
        break;
    }
}

static esp_err_t init_gap(void)
{
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t key_size = 16;

    ESP_RETURN_ON_ERROR(esp_ble_gap_register_callback(gap_callback), TAG, "register gap callback");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req)),
                        TAG, "set auth mode");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap)),
                        TAG, "set io capability");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key)),
                        TAG, "set init key");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key)),
                        TAG, "set response key");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size)),
                        TAG, "set max key size");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_device_name(VIBE_BOX_BLE_NAME), TAG, "set device name");
    ESP_RETURN_ON_ERROR(esp_ble_gap_config_adv_data(&adv_data), TAG, "config advertising");
    ESP_RETURN_ON_ERROR(set_new_random_address(), TAG, "set initial random address");
    return ESP_OK;
}

esp_err_t ble_keyboard_init(void)
{
    if (s_started) {
        return ESP_OK;
    }

#if CONFIG_BT_CLASSIC_ENABLED
    ESP_RETURN_ON_ERROR(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT), TAG, "release classic bt memory");
#endif

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&bt_cfg), TAG, "bt controller init");
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_BLE), TAG, "bt controller enable");
    ESP_RETURN_ON_ERROR(esp_bluedroid_init(), TAG, "bluedroid init");
    ESP_RETURN_ON_ERROR(esp_bluedroid_enable(), TAG, "bluedroid enable");
    ESP_RETURN_ON_ERROR(init_gap(), TAG, "gap init");
    ESP_RETURN_ON_ERROR(esp_ble_gatts_register_callback(gatts_dispatcher), TAG, "gatts callback");
    ESP_RETURN_ON_ERROR(esp_hidd_dev_init(&hid_config, ESP_HID_TRANSPORT_BLE, hidd_event_callback, &s_hid_dev),
                        TAG, "hid device init");
    ESP_RETURN_ON_ERROR(esp_ble_gatts_app_register(TEXT_GATTS_APP_ID), TAG, "register text service");

    s_started = true;
    ESP_LOGI(TAG, "BLE keyboard initialized");
    return ESP_OK;
}

bool ble_keyboard_is_connected(void)
{
    return s_hid_dev != NULL && esp_hidd_dev_connected(s_hid_dev);
}

bool ble_keyboard_text_client_connected(void)
{
    return s_text_connected;
}

bool ble_keyboard_text_notify_enabled(void)
{
    return s_text_connected && s_text_notify_enabled;
}

bool ble_keyboard_config_notify_enabled(void)
{
    return s_text_connected && s_config_notify_enabled;
}

esp_err_t ble_keyboard_reinitialize(void)
{
    if (!s_started) {
        return ble_keyboard_init();
    }

    ESP_LOGI(TAG, "reinitializing BLE advertising/session state");

    if (s_peer_connected) {
        esp_err_t disconnect_err = esp_ble_gap_disconnect(s_peer_remote_bda);
        if (disconnect_err != ESP_OK) {
            ESP_LOGW(TAG, "disconnect BLE peer failed: %s", esp_err_to_name(disconnect_err));
        }
    }

    s_peer_connected = false;
    s_text_connected = false;
    s_text_notify_enabled = false;
    s_config_notify_enabled = false;
    s_config_rx_len = 0;
    memset(s_peer_remote_bda, 0, sizeof(s_peer_remote_bda));
    memset(s_text_remote_bda, 0, sizeof(s_text_remote_bda));

    esp_err_t stop_err = esp_ble_gap_stop_advertising();
    if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "stop advertising failed: %s", esp_err_to_name(stop_err));
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    return set_new_random_address();
}

esp_err_t ble_keyboard_send_key(uint8_t modifier, uint8_t keycode)
{
    if (!ble_keyboard_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t report[8] = {modifier, 0, keycode, 0, 0, 0, 0, 0};
    ESP_RETURN_ON_ERROR(esp_hidd_dev_input_set(s_hid_dev, 0, HID_REPORT_ID_KEYBOARD,
                                               report, sizeof(report)),
                        TAG, "send key");
    vTaskDelay(pdMS_TO_TICKS(40));
    memset(report, 0, sizeof(report));
    return esp_hidd_dev_input_set(s_hid_dev, 0, HID_REPORT_ID_KEYBOARD, report, sizeof(report));
}

static esp_err_t text_notify_packet(uint8_t opcode, const uint8_t *payload, size_t payload_len)
{
    if (!s_text_connected || !s_text_notify_enabled || s_text_gatts_if == ESP_GATT_IF_NONE ||
        s_text_handles[TEXT_IDX_CHAR_VAL] == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (payload_len > TEXT_NOTIFY_CHUNK_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t packet[TEXT_NOTIFY_MTU_PAYLOAD_MAX] = {opcode};
    if (payload_len > 0) {
        memcpy(&packet[1], payload, payload_len);
    }

    return esp_ble_gatts_send_indicate(s_text_gatts_if, s_text_conn_id,
                                       s_text_handles[TEXT_IDX_CHAR_VAL],
                                       payload_len + 1, packet, false);
}

esp_err_t ble_keyboard_notify_text(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(text);
    esp_err_t err = text_notify_packet(TEXT_OPCODE_BEGIN, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t *cursor = (const uint8_t *)text;
    while (len > 0) {
        size_t chunk_len = len > TEXT_NOTIFY_CHUNK_MAX ? TEXT_NOTIFY_CHUNK_MAX : len;
        err = text_notify_packet(TEXT_OPCODE_CHUNK, cursor, chunk_len);
        if (err != ESP_OK) {
            return err;
        }
        cursor += chunk_len;
        len -= chunk_len;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return text_notify_packet(TEXT_OPCODE_END, NULL, 0);
}
