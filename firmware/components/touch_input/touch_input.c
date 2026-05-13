#include "touch_input.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "touch_input";

/* Touch register map (subset). On both FT6336 (Waveshare V2 default, 0x38)
 * and CST816T (0x15) register 0x02 holds the current finger count (low
 * nibble), which is all we need for the press/release detection used by
 * the press-and-hold trigger. */
#define TOUCH_REG_FINGER_NUM   0x02
#define TOUCH_PROBE_TIMEOUT_MS 50
#define TOUCH_I2C_TIMEOUT_MS   50

#define DEFAULT_I2C_SPEED_HZ     100000U
#define DEFAULT_POLL_PERIOD_MS   20U
#define DEFAULT_DEBOUNCE_SAMPLES 2U
#define DEFAULT_I2C_ADDR         0x38

typedef struct {
    touch_input_config_t cfg;
    touch_event_cb_t     cb;
    void                *user_ctx;

    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;

    bool pressed;
} touch_state_t;

static touch_state_t s_state;
static bool s_initialized;

static esp_err_t ensure_i2c_bus(touch_state_t *st)
{
    /* Audio codec module owns/initializes the same I2C port. Use the existing
     * bus handle if it is already up; otherwise create the bus ourselves so
     * the touch IC works even before the first recording attempt. */
    esp_err_t err = i2c_master_get_bus_handle(st->cfg.i2c_port, &st->bus);
    if (err == ESP_OK && st->bus != NULL) {
        ESP_LOGI(TAG, "reusing existing I2C bus on port %d", st->cfg.i2c_port);
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = st->cfg.i2c_port,
        .sda_io_num = st->cfg.i2c_sda_gpio,
        .scl_io_num = st->cfg.i2c_scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &st->bus),
                        TAG, "i2c_new_master_bus failed");
    ESP_LOGI(TAG, "created new I2C bus port=%d sda=%d scl=%d",
             st->cfg.i2c_port, st->cfg.i2c_sda_gpio, st->cfg.i2c_scl_gpio);
    return ESP_OK;
}

static esp_err_t reset_touch_panel(touch_state_t *st)
{
    if (st->cfg.reset_gpio < 0) {
        return ESP_OK;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << st->cfg.reset_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "reset gpio config");

    /* CST816T reset: low >= 1ms, then high, wait ~50ms for boot. */
    gpio_set_level(st->cfg.reset_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(st->cfg.reset_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(st->cfg.reset_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(60));
    return ESP_OK;
}

static esp_err_t configure_int_pin(touch_state_t *st)
{
    if (st->cfg.int_gpio < 0) {
        return ESP_OK;
    }
    /* INT is informational for now; we poll. Configure as input with pull-up
     * so it stays high when idle and does not float. */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << st->cfg.int_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

static esp_err_t read_finger_num(touch_state_t *st, uint8_t *out)
{
    uint8_t reg = TOUCH_REG_FINGER_NUM;
    uint8_t raw = 0;
    esp_err_t err = i2c_master_transmit_receive(
        st->dev, &reg, 1, &raw, 1, TOUCH_I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        /* FT6336: low nibble = touch count (0..2), upper bits reserved.
         * CST816T: same byte holds 0/1. Mask to be safe. */
        *out = raw & 0x0F;
    }
    return err;
}

/* Scan the I2C bus and log every responding address. Helpful when the touch
 * IC part number (and therefore its address) is unknown. */
static void scan_i2c_bus(touch_state_t *st)
{
    ESP_LOGI(TAG, "I2C scan begin (port=%d)", st->cfg.i2c_port);
    int found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
        if (i2c_master_probe(st->bus, addr, 20) == ESP_OK) {
            ESP_LOGI(TAG, "  I2C device responded at 0x%02x", addr);
            found++;
        }
    }
    ESP_LOGI(TAG, "I2C scan end, %d device(s) found", found);
}

static void touch_task(void *arg)
{
    touch_state_t *st = (touch_state_t *)arg;
    const TickType_t period = pdMS_TO_TICKS(st->cfg.poll_period_ms);
    const uint8_t debounce_target = st->cfg.debounce_samples;

    bool stable_pressed = false;
    bool candidate_pressed = false;
    uint8_t candidate_count = 0;
    uint8_t consecutive_errors = 0;

    ESP_LOGI(TAG, "touch poll task running (period=%ums debounce=%u)",
             (unsigned)st->cfg.poll_period_ms, (unsigned)debounce_target);

    while (true) {
        uint8_t finger = 0;
        esp_err_t err = read_finger_num(st, &finger);
        bool sample_pressed;
        if (err == ESP_OK) {
            consecutive_errors = 0;
            sample_pressed = (finger > 0);
        } else {
            consecutive_errors++;
            /* Log the very first error and then at most once every ~10s to
             * avoid flooding the console while the chip is silent. */
            uint32_t throttle = 10000U / (st->cfg.poll_period_ms ? st->cfg.poll_period_ms : 20U);
            if (throttle == 0U) throttle = 1U;
            if (consecutive_errors == 1U || (consecutive_errors % throttle) == 0U) {
                ESP_LOGW(TAG, "finger num read failed: %s (consec=%u)",
                         esp_err_to_name(err), (unsigned)consecutive_errors);
            }
            /* Treat read errors as "not pressed" so we never get stuck in a
             * fake press that would block the BOOT button path. */
            sample_pressed = false;
        }

        if (sample_pressed == candidate_pressed) {
            if (candidate_count < debounce_target) {
                candidate_count++;
            }
        } else {
            candidate_pressed = sample_pressed;
            candidate_count = 1;
        }

        if (candidate_count >= debounce_target && candidate_pressed != stable_pressed) {
            stable_pressed = candidate_pressed;
            st->pressed = stable_pressed;
            if (st->cb != NULL) {
                st->cb(stable_pressed ? TOUCH_EVENT_DOWN : TOUCH_EVENT_UP,
                       st->user_ctx);
            }
        }

        vTaskDelay(period);
    }
}

esp_err_t touch_input_init(const touch_input_config_t *cfg,
                           touch_event_cb_t cb,
                           void *user_ctx)
{
    if (cfg == NULL || cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->i2c_port < 0 || cfg->i2c_sda_gpio < 0 || cfg->i2c_scl_gpio < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_state, 0, sizeof(s_state));
    s_state.cfg = *cfg;
    s_state.cb = cb;
    s_state.user_ctx = user_ctx;

    if (s_state.cfg.i2c_addr == 0U) {
        s_state.cfg.i2c_addr = DEFAULT_I2C_ADDR;
    }
    if (s_state.cfg.i2c_speed_hz == 0U) {
        s_state.cfg.i2c_speed_hz = DEFAULT_I2C_SPEED_HZ;
    }
    if (s_state.cfg.poll_period_ms == 0U) {
        s_state.cfg.poll_period_ms = DEFAULT_POLL_PERIOD_MS;
    }
    if (s_state.cfg.debounce_samples == 0U) {
        s_state.cfg.debounce_samples = DEFAULT_DEBOUNCE_SAMPLES;
    }

    ESP_RETURN_ON_ERROR(reset_touch_panel(&s_state), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(configure_int_pin(&s_state), TAG, "int gpio config");
    ESP_RETURN_ON_ERROR(ensure_i2c_bus(&s_state), TAG, "i2c bus init");

    /* Diagnostic scan: tells us what addresses are actually present on the
     * shared bus so we can confirm the touch IC's address (CST816T=0x15,
     * FT6336=0x38, GT911=0x14/0x5d, etc.). */
    scan_i2c_bus(&s_state);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = s_state.cfg.i2c_addr,
        .scl_speed_hz = s_state.cfg.i2c_speed_hz,
    };
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(s_state.bus, &dev_cfg, &s_state.dev),
        TAG, "add cst816t device");

    esp_err_t probe_err = i2c_master_probe(
        s_state.bus, s_state.cfg.i2c_addr, TOUCH_PROBE_TIMEOUT_MS);
    if (probe_err != ESP_OK) {
        ESP_LOGW(TAG, "touch probe at 0x%02x failed: %s (will keep polling anyway)",
                 s_state.cfg.i2c_addr, esp_err_to_name(probe_err));
    } else {
        ESP_LOGI(TAG, "touch IC probed OK at 0x%02x", s_state.cfg.i2c_addr);
    }

    /* The user-supplied callback runs in this task and may perform heavy
     * work (HTTPS upload, JSON parse, BLE notify). Match the BOOT button's
     * 6 KiB stack so the same recording/upload path fits without overflow. */
    BaseType_t ok = xTaskCreatePinnedToCore(
        touch_task, "touch_btn", 6144, &s_state, 5, NULL, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create touch_task");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    return ESP_OK;
}

bool touch_input_is_pressed(void)
{
    return s_initialized && s_state.pressed;
}
