/*
 * ui_epaper - minimal driver for the Waveshare ESP32-S3-Touch-ePaper-1.54 V2.
 *
 * Implements just enough of the SSD1681 / 1.54-inch e-paper command set to
 * draw status pages with the bundled 8x8 ASCII font. Ported from the C++
 * sample at:
 *   .../09_LVGL_V8_Test/components/epaper_driver_bsp
 *   .../09_LVGL_V8_Test/components/board_power_bsp
 */

#include "ui_epaper.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "font8x8_basic.h"

static const char *TAG = "ui_epaper";

/* Pin map taken from the Waveshare V2 sample user_config.h */
#define EPD_DC_PIN    GPIO_NUM_10
#define EPD_CS_PIN    GPIO_NUM_11
#define EPD_SCK_PIN   GPIO_NUM_12
#define EPD_MOSI_PIN  GPIO_NUM_13
#define EPD_RST_PIN   GPIO_NUM_9
#define EPD_BUSY_PIN  GPIO_NUM_8
#define EPD_PWR_PIN   GPIO_NUM_6
#define VBAT_PWR_PIN  GPIO_NUM_17

#define EPD_SPI_HOST  SPI2_HOST
#define EPD_BUF_LEN   UI_EPAPER_FRAME_BUFFER_BYTES /* 25 * 200 = 5000 */
/* Poll period must round up to at least one FreeRTOS tick or vTaskDelay()
 * returns immediately and the timeout becomes meaningless. We measure real
 * wall-clock elapsed time via esp_timer_get_time() instead of summing the
 * requested delays. */
#define EPD_BUSY_POLL_PERIOD_MS 10
#define EPD_BUSY_TIMEOUT_MS     15000

#define EPD_COLOR_WHITE 0xFF
#define EPD_COLOR_BLACK 0x00

#define FONT_GLYPH_W 8
#define FONT_GLYPH_H 8

static spi_device_handle_t s_spi;
static uint8_t *s_frame_buffer;
static bool s_initialized;

/* Full-refresh waveform (matches Waveshare V2 sample). */
static const uint8_t WF_Full_1IN54[159] = {
    0x80, 0x48, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x48, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x48, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x48, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x01, 0x00, 0x08, 0x01, 0x00, 0x02,
    0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00,
    0x22, 0x17, 0x41, 0x00, 0x32, 0x20,
};

/* Partial-refresh waveform. */
static const uint8_t WF_PARTIAL_1IN54_0[159] = {
    0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00,
    0x02, 0x17, 0x41, 0xB0, 0x32, 0x28,
};

/* ------------------------------------------------------------------ */
/* Low-level helpers                                                  */
/* ------------------------------------------------------------------ */

static inline void epd_dc(int level)  { gpio_set_level(EPD_DC_PIN, level); }
static inline void epd_cs(int level)  { gpio_set_level(EPD_CS_PIN, level); }
static inline void epd_rst(int level) { gpio_set_level(EPD_RST_PIN, level); }

static esp_err_t epd_wait_busy(void)
{
    TickType_t poll_ticks = pdMS_TO_TICKS(EPD_BUSY_POLL_PERIOD_MS);
    if (poll_ticks == 0) {
        poll_ticks = 1;
    }
    int64_t start_us = esp_timer_get_time();
    while (gpio_get_level(EPD_BUSY_PIN) == 1) {
        vTaskDelay(poll_ticks);
        int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
        if (elapsed_ms >= EPD_BUSY_TIMEOUT_MS) {
            ESP_LOGE(TAG, "BUSY pin stuck high for %lldms", (long long)elapsed_ms);
            return ESP_ERR_TIMEOUT;
        }
    }
    int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
    if (elapsed_ms > 200) {
        ESP_LOGD(TAG, "BUSY released after %lldms", (long long)elapsed_ms);
    }
    return ESP_OK;
}

static void epd_spi_write(const uint8_t *data, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void epd_send_command(uint8_t cmd)
{
    epd_dc(0);
    epd_cs(0);
    epd_spi_write(&cmd, 1);
    epd_cs(1);
}

static void epd_send_data(uint8_t data)
{
    epd_dc(1);
    epd_cs(0);
    epd_spi_write(&data, 1);
    epd_cs(1);
}

static void epd_send_data_buf(const uint8_t *data, size_t len)
{
    epd_dc(1);
    epd_cs(0);
    epd_spi_write(data, len);
    epd_cs(1);
}

static void epd_set_window(uint16_t x_start, uint16_t y_start,
                           uint16_t x_end, uint16_t y_end)
{
    epd_send_command(0x44);
    epd_send_data((x_start >> 3) & 0xFF);
    epd_send_data((x_end   >> 3) & 0xFF);

    epd_send_command(0x45);
    epd_send_data(y_start & 0xFF);
    epd_send_data((y_start >> 8) & 0xFF);
    epd_send_data(y_end   & 0xFF);
    epd_send_data((y_end   >> 8) & 0xFF);
}

static void epd_set_cursor(uint16_t x, uint16_t y)
{
    epd_send_command(0x4E);
    epd_send_data(x & 0xFF);

    epd_send_command(0x4F);
    epd_send_data(y & 0xFF);
    epd_send_data((y >> 8) & 0xFF);
}

static void epd_set_lut(const uint8_t *lut)
{
    epd_send_command(0x32);
    epd_send_data_buf(lut, 153);
    (void)epd_wait_busy();

    epd_send_command(0x3F);
    epd_send_data(lut[153]);

    epd_send_command(0x03);
    epd_send_data(lut[154]);

    epd_send_command(0x04);
    epd_send_data(lut[155]);
    epd_send_data(lut[156]);
    epd_send_data(lut[157]);

    epd_send_command(0x2C);
    epd_send_data(lut[158]);
}

static void epd_turn_on_full(void)
{
    epd_send_command(0x22);
    epd_send_data(0xC7);
    epd_send_command(0x20);
    (void)epd_wait_busy();
}

static void epd_turn_on_partial(void)
{
    epd_send_command(0x22);
    epd_send_data(0xCF);
    epd_send_command(0x20);
    (void)epd_wait_busy();
}

/* ------------------------------------------------------------------ */
/* Init / power                                                       */
/* ------------------------------------------------------------------ */

static esp_err_t power_init(void)
{
    gpio_config_t cfg = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << EPD_PWR_PIN) | (1ULL << VBAT_PWR_PIN),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "power gpio config");

    /* EPD_PWR_PIN is active-low; VBAT enable is active-high. */
    gpio_set_level(EPD_PWR_PIN, 0);
    gpio_set_level(VBAT_PWR_PIN, 1);
    /* The panel's onboard regulator needs a moment to come up before we
     * start poking RST / SPI. Without this the first BUSY poll can hang. */
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "panel power rail enabled (PWR=GPIO%d low, VBAT=GPIO%d high)",
             EPD_PWR_PIN, VBAT_PWR_PIN);
    return ESP_OK;
}

static esp_err_t spi_init(void)
{
    gpio_config_t out_cfg = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << EPD_RST_PIN) | (1ULL << EPD_DC_PIN) | (1ULL << EPD_CS_PIN),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out_cfg), TAG, "epd ctrl gpio config");

    gpio_config_t in_cfg = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << EPD_BUSY_PIN),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&in_cfg), TAG, "epd busy gpio config");

    epd_rst(1);

    spi_bus_config_t bus = {
        .miso_io_num = -1,
        .mosi_io_num = EPD_MOSI_PIN,
        .sclk_io_num = EPD_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = UI_EPAPER_WIDTH * UI_EPAPER_HEIGHT,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "spi bus init");

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 40 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 7,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(EPD_SPI_HOST, &dev, &s_spi),
                        TAG, "spi device add");
    return ESP_OK;
}

static esp_err_t panel_init_full(void)
{
    epd_rst(1);
    vTaskDelay(pdMS_TO_TICKS(50));
    epd_rst(0);
    vTaskDelay(pdMS_TO_TICKS(20));
    epd_rst(1);
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_RETURN_ON_ERROR(epd_wait_busy(), TAG, "wait busy after reset");
    epd_send_command(0x12); /* SWRESET */
    ESP_RETURN_ON_ERROR(epd_wait_busy(), TAG, "wait busy after swreset");

    epd_send_command(0x01); /* Driver output control */
    epd_send_data(0xC7);
    epd_send_data(0x00);
    epd_send_data(0x01);

    epd_send_command(0x11); /* data entry mode */
    epd_send_data(0x01);

    epd_set_window(0, UI_EPAPER_WIDTH - 1, UI_EPAPER_HEIGHT - 1, 0);

    epd_send_command(0x3C); /* BorderWaveform */
    epd_send_data(0x01);

    epd_send_command(0x18);
    epd_send_data(0x80);

    epd_send_command(0x22); /* Load temperature & waveform */
    epd_send_data(0xB1);
    epd_send_command(0x20);

    epd_set_cursor(0, UI_EPAPER_HEIGHT - 1);
    ESP_RETURN_ON_ERROR(epd_wait_busy(), TAG, "wait busy after cursor");

    epd_set_lut(WF_Full_1IN54);
    return ESP_OK;
}

static esp_err_t panel_init_partial(void)
{
    epd_rst(1);
    vTaskDelay(pdMS_TO_TICKS(50));
    epd_rst(0);
    vTaskDelay(pdMS_TO_TICKS(20));
    epd_rst(1);
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_RETURN_ON_ERROR(epd_wait_busy(), TAG, "wait busy partial reset");

    epd_set_lut(WF_PARTIAL_1IN54_0);

    epd_send_command(0x37);
    for (int i = 0; i < 10; ++i) {
        epd_send_data(i == 5 ? 0x40 : 0x00);
    }

    epd_send_command(0x3C); /* BorderWaveform */
    epd_send_data(0x80);

    epd_send_command(0x22);
    epd_send_data(0xC0);
    epd_send_command(0x20);
    return epd_wait_busy();
}

static esp_err_t push_full(void)
{
    epd_send_command(0x24);
    epd_send_data_buf(s_frame_buffer, EPD_BUF_LEN);
    epd_send_command(0x26);
    epd_send_data_buf(s_frame_buffer, EPD_BUF_LEN);
    epd_turn_on_full();
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

esp_err_t ui_epaper_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_frame_buffer = heap_caps_malloc(EPD_BUF_LEN, MALLOC_CAP_SPIRAM);
    if (s_frame_buffer == NULL) {
        s_frame_buffer = heap_caps_malloc(EPD_BUF_LEN, MALLOC_CAP_8BIT);
    }
    if (s_frame_buffer == NULL) {
        ESP_LOGE(TAG, "frame buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    memset(s_frame_buffer, EPD_COLOR_WHITE, EPD_BUF_LEN);

    ESP_LOGI(TAG, "init: starting power/SPI bring-up");
    ESP_RETURN_ON_ERROR(power_init(), TAG, "power init");
    ESP_RETURN_ON_ERROR(spi_init(),   TAG, "spi init");
    ESP_LOGI(TAG, "init: SPI ready, running full panel init");
    ESP_RETURN_ON_ERROR(panel_init_full(), TAG, "panel init full");
    ESP_LOGI(TAG, "init: full init OK, pushing white baseline");

    /* Lay down a clean white baseline before switching into partial mode. */
    (void)push_full();
    ESP_LOGI(TAG, "init: baseline pushed, switching to partial mode");
    ESP_RETURN_ON_ERROR(panel_init_partial(), TAG, "panel init partial");

    s_initialized = true;
    ESP_LOGI(TAG, "ready (%dx%d, buffer=%d bytes)",
             UI_EPAPER_WIDTH, UI_EPAPER_HEIGHT, EPD_BUF_LEN);
    return ESP_OK;
}

bool ui_epaper_is_ready(void)
{
    return s_initialized;
}

void ui_epaper_clear(void)
{
    if (s_frame_buffer == NULL) {
        return;
    }
    memset(s_frame_buffer, EPD_COLOR_WHITE, EPD_BUF_LEN);
}

static void draw_pixel_black(int x, int y)
{
    if (x < 0 || x >= UI_EPAPER_WIDTH || y < 0 || y >= UI_EPAPER_HEIGHT) {
        return;
    }
    int row_bytes = UI_EPAPER_WIDTH / 8;
    int idx = y * row_bytes + (x >> 3);
    uint8_t mask = 0x80 >> (x & 0x07);
    s_frame_buffer[idx] &= (uint8_t)~mask;
}

static void draw_glyph(int x, int y, unsigned char ch)
{
    if (ch >= 128) {
        ch = '?';
    }
    const unsigned char *glyph = font8x8_basic[ch];
    for (int row = 0; row < FONT_GLYPH_H; ++row) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_GLYPH_W; ++col) {
            /* Font bit 0 == leftmost pixel. */
            if (bits & (1u << col)) {
                draw_pixel_black(x + col, y + row);
            }
        }
    }
}

void ui_epaper_draw_text(int x, int y, const char *text)
{
    if (s_frame_buffer == NULL || text == NULL) {
        return;
    }
    int cursor_x = x;
    while (*text != '\0') {
        unsigned char c = (unsigned char)*text++;
        if (c == '\n' || cursor_x + FONT_GLYPH_W > UI_EPAPER_WIDTH) {
            cursor_x = x;
            y += FONT_GLYPH_H;
            if (c == '\n') {
                continue;
            }
        }
        if (y + FONT_GLYPH_H > UI_EPAPER_HEIGHT) {
            return;
        }
        draw_glyph(cursor_x, y, c);
        cursor_x += FONT_GLYPH_W;
    }
}

void ui_epaper_draw_hline(int y, int x0, int x1)
{
    if (s_frame_buffer == NULL) {
        return;
    }
    if (x0 > x1) {
        int tmp = x0; x0 = x1; x1 = tmp;
    }
    for (int x = x0; x <= x1; ++x) {
        draw_pixel_black(x, y);
    }
}

void ui_epaper_draw_vline(int x, int y0, int y1)
{
    if (s_frame_buffer == NULL) {
        return;
    }
    if (y0 > y1) {
        int tmp = y0; y0 = y1; y1 = tmp;
    }
    for (int y = y0; y <= y1; ++y) {
        draw_pixel_black(x, y);
    }
}

void ui_epaper_draw_rect(int x0, int y0, int x1, int y1)
{
    ui_epaper_draw_hline(y0, x0, x1);
    ui_epaper_draw_hline(y1, x0, x1);
    ui_epaper_draw_vline(x0, y0, y1);
    ui_epaper_draw_vline(x1, y0, y1);
}

esp_err_t ui_epaper_flush(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Match the Waveshare V2 sample's EPD_DisplayPart() exactly: write the
     * new frame to RAM and trigger a partial refresh. The "previous" RAM is
     * left alone so the controller diffs the next frame against what is
     * currently on the panel. */
    epd_send_command(0x24);
    epd_send_data_buf(s_frame_buffer, EPD_BUF_LEN);
    epd_turn_on_partial();
    return ESP_OK;
}

esp_err_t ui_epaper_show_bitmap(const uint8_t *bitmap, size_t bitmap_len)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bitmap == NULL || bitmap_len != EPD_BUF_LEN || s_frame_buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(s_frame_buffer, bitmap, EPD_BUF_LEN);
    return ui_epaper_flush();
}

/* ------------------------------------------------------------------ */
/* High-level rendering helpers                                       */
/* ------------------------------------------------------------------ */

static void render_wrapped(int x, int *y, int max_width_chars, const char *text)
{
    if (text == NULL || *text == '\0') {
        return;
    }
    char line[UI_EPAPER_MAX_COLS + 1];
    size_t len = strlen(text);
    size_t pos = 0;
    while (pos < len) {
        size_t take = len - pos;
        if ((int)take > max_width_chars) {
            take = (size_t)max_width_chars;
        }
        memcpy(line, text + pos, take);
        line[take] = '\0';
        ui_epaper_draw_text(x, *y, line);
        *y += FONT_GLYPH_H;
        pos += take;
        if (*y + FONT_GLYPH_H > UI_EPAPER_HEIGHT) {
            return;
        }
    }
}

esp_err_t ui_epaper_show_status(const char *headline, const char *detail)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    ui_epaper_clear();

    int y = 4;
    if (headline != NULL && headline[0] != '\0') {
        ui_epaper_draw_text(4, y, headline);
        y += FONT_GLYPH_H + 2;
        ui_epaper_draw_hline(y, 4, UI_EPAPER_WIDTH - 5);
        y += 4;
    }

    render_wrapped(4, &y, (UI_EPAPER_WIDTH - 8) / FONT_GLYPH_W, detail);
    return ui_epaper_flush();
}

esp_err_t ui_epaper_show_lines(const char *headline,
                               const char *const *lines,
                               size_t line_count)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    ui_epaper_clear();

    int y = 4;
    if (headline != NULL && headline[0] != '\0') {
        ui_epaper_draw_text(4, y, headline);
        y += FONT_GLYPH_H + 2;
        ui_epaper_draw_hline(y, 4, UI_EPAPER_WIDTH - 5);
        y += 4;
    }

    int max_chars = (UI_EPAPER_WIDTH - 8) / FONT_GLYPH_W;
    for (size_t i = 0; i < line_count && i < UI_EPAPER_MAX_LINES; ++i) {
        render_wrapped(4, &y, max_chars, lines[i]);
        if (y + FONT_GLYPH_H > UI_EPAPER_HEIGHT) {
            break;
        }
    }
    return ui_epaper_flush();
}
