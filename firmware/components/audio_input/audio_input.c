#include "audio_input.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "audio_input";

#define ES8311_I2C_TIMEOUT_MS          1000
#define ES8311_RESET_REG00             0x00
#define ES8311_CLK_MANAGER_REG01       0x01
#define ES8311_CLK_MANAGER_REG02       0x02
#define ES8311_CLK_MANAGER_REG03       0x03
#define ES8311_CLK_MANAGER_REG04       0x04
#define ES8311_CLK_MANAGER_REG05       0x05
#define ES8311_CLK_MANAGER_REG06       0x06
#define ES8311_CLK_MANAGER_REG07       0x07
#define ES8311_CLK_MANAGER_REG08       0x08
#define ES8311_SDPIN_REG09             0x09
#define ES8311_SDPOUT_REG0A            0x0A
#define ES8311_SYSTEM_REG0D            0x0D
#define ES8311_SYSTEM_REG0E            0x0E
#define ES8311_SYSTEM_REG12            0x12
#define ES8311_SYSTEM_REG14            0x14
#define ES8311_SYSTEM_REG15            0x15
#define ES8311_ADC_REG16               0x16
#define ES8311_ADC_REG17               0x17
#define ES8311_ADC_REG18               0x18
#define ES8311_ADC_REG19               0x19
#define ES8311_ADC_REG1A               0x1A
#define ES8311_ADC_REG1B               0x1B
#define ES8311_ADC_REG1C               0x1C
#define ES8311_DAC_REG37               0x37
#define ES8311_GPIO_REG44              0x44
#define ES8311_GP_REG45                0x45
#define ES8311_CODEC_DEFAULT_ADDR      0x18

static i2c_master_bus_handle_t s_i2c_bus_handle;
static i2c_master_dev_handle_t s_codec_dev_handle;
static audio_input_i2s_config_t s_codec_cfg;
static bool s_codec_ready;

static bool capture_config_equals(const audio_input_i2s_config_t *lhs,
                                  const audio_input_i2s_config_t *rhs)
{
    return memcmp(lhs, rhs, sizeof(*lhs)) == 0;
}

static esp_err_t configure_output_pin(int gpio_num, int level)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << (uint32_t)gpio_num,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio_config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(gpio_num, level), TAG, "gpio_set_level failed");
    return ESP_OK;
}

static esp_err_t es8311_write_reg(uint8_t reg_addr, uint8_t value)
{
    uint8_t payload[2] = {reg_addr, value};

    if (s_codec_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return i2c_master_transmit(
        s_codec_dev_handle, payload, sizeof(payload), ES8311_I2C_TIMEOUT_MS);
}

static esp_err_t es8311_read_reg(uint8_t reg_addr, uint8_t *value_out)
{
    if (s_codec_dev_handle == NULL || value_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return i2c_master_transmit_receive(
        s_codec_dev_handle, &reg_addr, sizeof(reg_addr), value_out, 1, ES8311_I2C_TIMEOUT_MS);
}

static esp_err_t es8311_update_reg(uint8_t reg_addr, uint8_t mask, uint8_t value)
{
    uint8_t reg_value = 0;

    ESP_RETURN_ON_ERROR(es8311_read_reg(reg_addr, &reg_value), TAG, "read reg failed");
    reg_value = (uint8_t)((reg_value & (uint8_t)(~mask)) | (value & mask));
    return es8311_write_reg(reg_addr, reg_value);
}

static esp_err_t es8311_configure_clocks_16k(void)
{
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG02, 0x00), TAG, "reg02 failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG03, 0x10), TAG, "reg03 failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG04, 0x20), TAG, "reg04 failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG05, 0x00), TAG, "reg05 failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG06, 0x03), TAG, "reg06 failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG07, 0x00), TAG, "reg07 failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG08, 0xFF), TAG, "reg08 failed");
    return ESP_OK;
}

static esp_err_t es8311_init_registers(const audio_input_i2s_config_t *cfg)
{
    uint8_t reg_value = 0;

    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_RESET_REG00, 0x1F), TAG, "reset stage1 failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_RESET_REG00, 0x00), TAG, "reset stage2 failed");

    ESP_RETURN_ON_ERROR(es8311_read_reg(ES8311_RESET_REG00, &reg_value), TAG, "read reg00 failed");
    reg_value = (uint8_t)(reg_value & (uint8_t)~0x40U);
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_RESET_REG00, reg_value), TAG, "slave mode failed");

    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG01, 0x30), TAG, "reg01 failed");
    ESP_RETURN_ON_ERROR(es8311_read_reg(ES8311_CLK_MANAGER_REG01, &reg_value), TAG, "read reg01 failed");
    reg_value = (uint8_t)(reg_value & (uint8_t)~0x40U);
    if (cfg->mclk_gpio >= 0) {
        reg_value = (uint8_t)(reg_value & (uint8_t)~0x80U);
    }
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG01, reg_value), TAG, "write reg01 failed");

    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG06, 0x00), TAG, "reg06 init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG07, 0x10), TAG, "reg07 init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG08, 0x10), TAG, "reg08 init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SDPIN_REG09, 0x3C), TAG, "reg09 init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SDPOUT_REG0A, 0x3C), TAG, "reg0a init failed");

    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SYSTEM_REG0D, 0xFF), TAG, "reg0d init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_ADC_REG16, 0x24), TAG, "reg16 init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_ADC_REG17, 0x00), TAG, "reg17 init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_ADC_REG18, 0x88), TAG, "reg18 init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_ADC_REG19, 0x02), TAG, "reg19 init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_ADC_REG1A, 0x0A), TAG, "reg1a init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_ADC_REG1B, 0x7F), TAG, "reg1b init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_ADC_REG1C, 0x6A), TAG, "reg1c init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_DAC_REG37, 0x48), TAG, "reg37 init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_GPIO_REG44, 0x00), TAG, "reg44 init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_GP_REG45, 0x00), TAG, "reg45 init failed");

    ESP_RETURN_ON_ERROR(es8311_configure_clocks_16k(), TAG, "clock config failed");
    return ESP_OK;
}

static esp_err_t es8311_start_adc(void)
{
    ESP_RETURN_ON_ERROR(es8311_update_reg(ES8311_SDPIN_REG09, 0x40, 0x00), TAG, "unmute adc failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SYSTEM_REG0E, 0x02), TAG, "reg0e start failed");
    ESP_RETURN_ON_ERROR(es8311_update_reg(ES8311_SYSTEM_REG12, 0x20, 0x00), TAG, "reg12 start failed");
    ESP_RETURN_ON_ERROR(es8311_update_reg(ES8311_SYSTEM_REG14, 0x40, 0x00), TAG, "reg14 start failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SYSTEM_REG0D, 0x00), TAG, "reg0d start failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SYSTEM_REG15, 0x00), TAG, "reg15 start failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_ADC_REG17, 0xBF), TAG, "reg17 start failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_GP_REG45, 0x00), TAG, "reg45 start failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_GPIO_REG44, 0x58), TAG, "reg44 start failed");
    return ESP_OK;
}

static void write_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xffU);
    dst[1] = (uint8_t)((value >> 8U) & 0xffU);
}

static void write_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xffU);
    dst[1] = (uint8_t)((value >> 8U) & 0xffU);
    dst[2] = (uint8_t)((value >> 16U) & 0xffU);
    dst[3] = (uint8_t)((value >> 24U) & 0xffU);
}

static esp_err_t write_wav_header(uint8_t *dst,
                                  size_t out_len,
                                  uint32_t sample_rate_hz,
                                  uint16_t channels,
                                  uint16_t bits_per_sample,
                                  size_t pcm_size)
{
    if (dst == NULL || out_len < 44U || sample_rate_hz == 0U || channels == 0U ||
        bits_per_sample != 16U || pcm_size > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(dst, 0, 44U);
    memcpy(dst + 0, "RIFF", 4);
    write_le32(dst + 4, (uint32_t)(pcm_size + 36U));
    memcpy(dst + 8, "WAVE", 4);
    memcpy(dst + 12, "fmt ", 4);
    write_le32(dst + 16, 16U);
    write_le16(dst + 20, 1U);
    write_le16(dst + 22, channels);
    write_le32(dst + 24, sample_rate_hz);
    write_le32(dst + 28, sample_rate_hz * channels * (bits_per_sample / 8U));
    write_le16(dst + 32, (uint16_t)(channels * (bits_per_sample / 8U)));
    write_le16(dst + 34, bits_per_sample);
    memcpy(dst + 36, "data", 4);
    write_le32(dst + 40, (uint32_t)pcm_size);
    return ESP_OK;
}

size_t audio_input_pcm_size(uint32_t sample_rate_hz,
                            uint16_t channels,
                            uint16_t bits_per_sample,
                            uint32_t duration_ms)
{
    uint64_t samples_per_channel;
    uint64_t total_bytes;

    if (sample_rate_hz == 0U || channels == 0U || bits_per_sample != 16U || duration_ms == 0U) {
        return 0;
    }

    samples_per_channel = ((uint64_t)sample_rate_hz * duration_ms) / 1000U;
    total_bytes = samples_per_channel * channels * (bits_per_sample / 8U);
    if (total_bytes > SIZE_MAX) {
        return 0;
    }

    return (size_t)total_bytes;
}

size_t audio_input_wav_size(uint32_t sample_rate_hz,
                            uint16_t channels,
                            uint16_t bits_per_sample,
                            uint32_t duration_ms)
{
    size_t pcm_size = audio_input_pcm_size(sample_rate_hz, channels, bits_per_sample, duration_ms);

    if (pcm_size == 0U) {
        return 0;
    }

    return 44U + pcm_size;
}

size_t audio_input_demo_pcm_size(const audio_input_demo_config_t *cfg)
{
    if (cfg == NULL) {
        return 0;
    }

    return audio_input_pcm_size(
        cfg->sample_rate_hz, cfg->channels, cfg->bits_per_sample, cfg->duration_ms);
}

size_t audio_input_demo_wav_size(const audio_input_demo_config_t *cfg)
{
    if (cfg == NULL) {
        return 0;
    }

    return audio_input_wav_size(
        cfg->sample_rate_hz, cfg->channels, cfg->bits_per_sample, cfg->duration_ms);
}

esp_err_t audio_input_generate_demo_wav(const audio_input_demo_config_t *cfg,
                                        uint8_t *out_buf,
                                        size_t out_len,
                                        size_t *written_out)
{
    size_t pcm_size;
    size_t wav_size;
    uint32_t sample_count;
    uint32_t sample_index;
    int16_t *pcm_dst;

    if (cfg == NULL || out_buf == NULL || written_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    pcm_size = audio_input_demo_pcm_size(cfg);
    wav_size = audio_input_demo_wav_size(cfg);
    if (pcm_size == 0U || wav_size == 0U || out_len < wav_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_RETURN_ON_ERROR(write_wav_header(out_buf,
                                         out_len,
                                         cfg->sample_rate_hz,
                                         cfg->channels,
                                         cfg->bits_per_sample,
                                         pcm_size),
                        TAG,
                        "write demo wav header failed");

    pcm_dst = (int16_t *)(void *)(out_buf + 44U);
    sample_count = (uint32_t)((pcm_size / sizeof(int16_t)) / cfg->channels);
    for (sample_index = 0; sample_index < sample_count; ++sample_index) {
        float phase =
            (2.0f * (float)M_PI * cfg->tone_frequency_hz * (float)sample_index) /
            (float)cfg->sample_rate_hz;
        int16_t sample_value = (int16_t)(sinf(phase) * (float)cfg->amplitude);
        uint16_t channel_index;

        for (channel_index = 0; channel_index < cfg->channels; ++channel_index) {
            pcm_dst[(sample_index * cfg->channels) + channel_index] = sample_value;
        }
    }

    *written_out = wav_size;
    return ESP_OK;
}

esp_err_t audio_input_prepare_i2s_capture(const audio_input_i2s_config_t *cfg)
{
    i2c_master_bus_config_t bus_cfg = {0};
    i2c_device_config_t device_cfg = {0};
    esp_err_t err;

    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->i2c_port < 0 || cfg->i2c_sda_gpio < 0 || cfg->i2c_scl_gpio < 0 ||
        cfg->codec_i2c_addr <= 0 || cfg->codec_i2c_addr > 0x7f || cfg->mclk_gpio < 0 ||
        cfg->bclk_gpio < 0 || cfg->ws_gpio < 0 || cfg->din_gpio < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_codec_ready && capture_config_equals(cfg, &s_codec_cfg)) {
        return ESP_OK;
    }

    if (cfg->pa_enable_gpio >= 0) {
        ESP_RETURN_ON_ERROR(configure_output_pin(cfg->pa_enable_gpio, 0), TAG, "pa_enable init failed");
    }
    if (cfg->pa_control_gpio >= 0) {
        ESP_RETURN_ON_ERROR(configure_output_pin(cfg->pa_control_gpio, 0), TAG, "pa_control init failed");
    }

    if (s_i2c_bus_handle == NULL) {
        bus_cfg.i2c_port = cfg->i2c_port;
        bus_cfg.sda_io_num = cfg->i2c_sda_gpio;
        bus_cfg.scl_io_num = cfg->i2c_scl_gpio;
        bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_cfg.glitch_ignore_cnt = 7;
        bus_cfg.flags.enable_internal_pullup = true;
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus_handle), TAG, "i2c bus init failed");
    }

    if (s_codec_dev_handle == NULL) {
        device_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        device_cfg.device_address = (uint16_t)cfg->codec_i2c_addr;
        device_cfg.scl_speed_hz = 100000;
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus_handle, &device_cfg, &s_codec_dev_handle),
                            TAG,
                            "codec device add failed");
    }

    err = i2c_master_probe(
        s_i2c_bus_handle, (uint16_t)cfg->codec_i2c_addr, ES8311_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "es8311 probe failed addr=0x%02x err=%s",
                 cfg->codec_i2c_addr,
                 esp_err_to_name(err));
        return err;
    }

    ESP_RETURN_ON_ERROR(es8311_init_registers(cfg), TAG, "es8311 init failed");
    ESP_RETURN_ON_ERROR(es8311_start_adc(), TAG, "es8311 adc start failed");

    s_codec_cfg = *cfg;
    s_codec_ready = true;
    ESP_LOGI(TAG,
             "es8311 ready i2c_port=%d sda=%d scl=%d addr=0x%02x mclk=%d bclk=%d ws=%d din=%d",
             cfg->i2c_port,
             cfg->i2c_sda_gpio,
             cfg->i2c_scl_gpio,
             cfg->codec_i2c_addr,
             cfg->mclk_gpio,
             cfg->bclk_gpio,
             cfg->ws_gpio,
             cfg->din_gpio);
    return ESP_OK;
}

esp_err_t audio_input_capture_i2s_wav(const audio_input_i2s_config_t *cfg,
                                      uint8_t *out_buf,
                                      size_t out_len,
                                      size_t *written_out)
{
    TickType_t read_timeout = pdMS_TO_TICKS(1000);
    size_t pcm_size;
    size_t wav_size;
    i2s_chan_handle_t rx_handle = NULL;
    i2s_chan_config_t chan_cfg;
    i2s_std_config_t std_cfg;
    size_t captured = 0;
    esp_err_t ret = ESP_OK;

    if (cfg == NULL || out_buf == NULL || written_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->bclk_gpio < 0 || cfg->ws_gpio < 0 || cfg->din_gpio < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->bits_per_sample != 16U || (cfg->channels != 1U && cfg->channels != 2U)) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(audio_input_prepare_i2s_capture(cfg), TAG, "prepare codec failed");

    pcm_size = audio_input_pcm_size(
        cfg->sample_rate_hz, cfg->channels, cfg->bits_per_sample, cfg->duration_ms);
    wav_size = audio_input_wav_size(
        cfg->sample_rate_hz, cfg->channels, cfg->bits_per_sample, cfg->duration_ms);
    if (pcm_size == 0U || wav_size == 0U || out_len < wav_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_RETURN_ON_ERROR(write_wav_header(out_buf,
                                         out_len,
                                         cfg->sample_rate_hz,
                                         cfg->channels,
                                         cfg->bits_per_sample,
                                         pcm_size),
                        TAG,
                        "write capture wav header failed");

    chan_cfg = (i2s_chan_config_t)I2S_CHANNEL_DEFAULT_CONFIG((i2s_port_t)cfg->i2s_port,
                                                             I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &rx_handle), TAG, "i2s_new_channel failed");

    std_cfg = (i2s_std_config_t){
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(cfg->sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            cfg->channels == 1U ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = cfg->mclk_gpio,
            .bclk = cfg->bclk_gpio,
            .ws = cfg->ws_gpio,
            .dout = I2S_GPIO_UNUSED,
            .din = cfg->din_gpio,
            .invert_flags =
                {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(rx_handle, &std_cfg), cleanup, TAG, "init std rx");
    ESP_GOTO_ON_ERROR(i2s_channel_enable(rx_handle), cleanup, TAG, "enable rx");

    ESP_LOGI(TAG,
             "starting i2s capture port=%d rate=%" PRIu32 " channels=%u duration_ms=%" PRIu32
             " bclk=%d ws=%d din=%d mclk=%d",
             cfg->i2s_port,
             cfg->sample_rate_hz,
             (unsigned)cfg->channels,
             cfg->duration_ms,
             cfg->bclk_gpio,
             cfg->ws_gpio,
             cfg->din_gpio,
             cfg->mclk_gpio);

    while (captured < pcm_size) {
        size_t bytes_read = 0;

        ret = i2s_channel_read(
            rx_handle, out_buf + 44U + captured, pcm_size - captured, &bytes_read, read_timeout);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s read failed: %s", esp_err_to_name(ret));
            goto cleanup;
        }
        if (bytes_read == 0U) {
            ret = ESP_ERR_TIMEOUT;
            ESP_LOGE(TAG, "i2s read returned zero bytes");
            goto cleanup;
        }
        captured += bytes_read;
    }

    *written_out = wav_size;
    ret = ESP_OK;

cleanup:
    if (rx_handle != NULL) {
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
    }
    return ret;
}
