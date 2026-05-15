#include "audio_input.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
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
#define ES8311_DAC_REG31               0x31
#define ES8311_DAC_REG32               0x32
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
    (void)cfg;

    /* Soft reset. */
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_RESET_REG00, 0x1F), TAG, "reset stage1 failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_RESET_REG00, 0x00), TAG, "reset stage2 failed");

    /* I2C noise-immunity double write (per esp_codec_dev es8311_open). */
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_GPIO_REG44, 0x08), TAG, "reg44 noise1 failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_GPIO_REG44, 0x08), TAG, "reg44 noise2 failed");

    /* Provisional clock/registers — configure_clocks_16k() rewrites the
     * dividers further down with the per-rate divider table values. */
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG01, 0x30), TAG, "reg01 init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG02, 0x00), TAG, "reg02 init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG03, 0x10), TAG, "reg03 init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_ADC_REG16, 0x24), TAG, "reg16 init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG04, 0x10), TAG, "reg04 init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG05, 0x00), TAG, "reg05 init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(0x0B, 0x00), TAG, "reg0b init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(0x0C, 0x00), TAG, "reg0c init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(0x10, 0x1F), TAG, "reg10 init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(0x11, 0x7F), TAG, "reg11 init failed");

    /* REG00 = 0x80 → CSM_ON = 1 (state machine running). Without this bit
     * the codec stays halted and the ADC produces digital zero forever. */
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_RESET_REG00, 0x80), TAG, "reg00 csm-on failed");
    ESP_RETURN_ON_ERROR(es8311_read_reg(ES8311_RESET_REG00, &reg_value), TAG, "read reg00 failed");
    reg_value = (uint8_t)(reg_value & 0xBFU);  /* slave mode: clear bit 6 (MS). */
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_RESET_REG00, reg_value), TAG, "reg00 slave failed");

    /* REG01 = 0x3F → use_mclk=1 (clear bit 7), invert_mclk=0 (clear bit 6),
     * and turn on the SYS_CLK + ADC_OSC + ADC_DMIC_OSC + DAC_OSC gates that
     * the previous 0x30 value left disabled. Missing ADC_OSC_EN was why the
     * ADC modulator had no oversample clock and produced silence. */
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG01, 0x3F), TAG, "reg01 mclk failed");

    /* SCLK polarity: invert_sclk = false → clear bit 5 of REG06. */
    ESP_RETURN_ON_ERROR(es8311_read_reg(ES8311_CLK_MANAGER_REG06, &reg_value), TAG, "read reg06 failed");
    reg_value = (uint8_t)(reg_value & (uint8_t)~0x20U);
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_CLK_MANAGER_REG06, reg_value), TAG, "reg06 polarity failed");

    /* Analog reference power-up and ADC anti-pop trim. */
    ESP_RETURN_ON_ERROR(es8311_write_reg(0x13, 0x10), TAG, "reg13 init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_ADC_REG1B, 0x0A), TAG, "reg1b init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_ADC_REG1C, 0x6A), TAG, "reg1c init failed");

    /* Internal reference routing for ADCL+DACR loopback default. */
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_GPIO_REG44, 0x58), TAG, "reg44 ref failed");

    /* SDP word-length + format: 16-bit / I2S Normal. Matches the final state
     * after canonical set_bits_per_sample(16) + config_fmt(I2S_NORMAL). */
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SDPIN_REG09, 0x0C), TAG, "reg09 init failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SDPOUT_REG0A, 0x0C), TAG, "reg0a init failed");

    /* Now apply the 16 kHz divider table values. */
    ESP_RETURN_ON_ERROR(es8311_configure_clocks_16k(), TAG, "clock config failed");
    return ESP_OK;
}

static esp_err_t es8311_start_adc(void)
{
    /* Mirrors esp_codec_dev es8311_start() for analog mic capture.
     * Unmute the ADC (REG0A bit 6, not REG09 which is the DAC), then power
     * up the analog ADC stages and route the analog mic into the PGA. */
    ESP_RETURN_ON_ERROR(es8311_update_reg(ES8311_SDPOUT_REG0A, 0x40, 0x00), TAG, "unmute adc failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_ADC_REG17, 0xBF), TAG, "reg17 pga gain failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SYSTEM_REG0E, 0x02), TAG, "reg0e analog pwr failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SYSTEM_REG12, 0x00), TAG, "reg12 pwr-up failed");
    /* REG14 = 0x1A: select analog MIC1, enable PGA, bit 6 = 0 → AMIC (not DMIC). */
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SYSTEM_REG14, 0x1A), TAG, "reg14 mic select failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SYSTEM_REG0D, 0x01), TAG, "reg0d digital pwr failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SYSTEM_REG15, 0x40), TAG, "reg15 adc cfg failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_DAC_REG37, 0x08), TAG, "reg37 failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_GP_REG45, 0x00), TAG, "reg45 failed");
    return ESP_OK;
}

static uint8_t es8311_dac_volume_reg(uint8_t volume_percent)
{
    if (volume_percent > 100U) {
        volume_percent = 100U;
    }

    return (uint8_t)(((uint16_t)volume_percent * 0xBFU + 50U) / 100U);
}

static esp_err_t es8311_start_dac(uint8_t volume_percent, bool muted)
{
    /* Mirrors es8311_start(ES_MODULE_DAC) from esp-adf: clear the SDP DAC
     * mute (REG09 bit 6) and the SDP ADC mute (REG0A bit 6), then power up
     * the analog/digital blocks. REG17 is part of the canonical bring-up
     * even in DAC-only mode -- without it the analog reference stays in a
     * post-reset state that produces silence on a fresh boot when no ADC
     * capture has primed the codec. */
    ESP_RETURN_ON_ERROR(es8311_update_reg(ES8311_SDPIN_REG09, 0x40, 0x00), TAG, "unmute dac failed");
    ESP_RETURN_ON_ERROR(es8311_update_reg(ES8311_SDPOUT_REG0A, 0x40, 0x00), TAG, "unmute adc sdp failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_ADC_REG17, 0xBF), TAG, "reg17 adc vol failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SYSTEM_REG0E, 0x02), TAG, "reg0e analog pwr failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SYSTEM_REG12, 0x00), TAG, "reg12 pwr-up failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SYSTEM_REG14, 0x1A), TAG, "reg14 dac failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SYSTEM_REG0D, 0x01), TAG, "reg0d digital pwr failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_SYSTEM_REG15, 0x40), TAG, "reg15 dac failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_DAC_REG37, 0x08), TAG, "reg37 dac failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_GP_REG45, 0x00), TAG, "reg45 dac failed");
    ESP_RETURN_ON_ERROR(es8311_write_reg(ES8311_DAC_REG32,
                                         es8311_dac_volume_reg(volume_percent)),
                        TAG,
                        "dac volume failed");
    ESP_RETURN_ON_ERROR(es8311_update_reg(ES8311_DAC_REG31,
                                          0x60,
                                          muted ? 0x60 : 0x00),
                        TAG,
                        "dac mute failed");
    return ESP_OK;
}

static void es8311_dump_registers(const char *label)
{
    static const uint8_t regs[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0A, 0x0D, 0x0E, 0x12, 0x13, 0x14, 0x15,
        0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C,
        0x31, 0x32, 0x37, 0x44, 0x45, 0xFD, 0xFE, 0xFF,
    };
    for (size_t i = 0; i < sizeof(regs); ++i) {
        uint8_t v = 0xAA;
        esp_err_t e = es8311_read_reg(regs[i], &v);
        if (e == ESP_OK) {
            ESP_LOGI(TAG, "es8311[%s] reg 0x%02x = 0x%02x", label, regs[i], v);
        } else {
            ESP_LOGW(TAG, "es8311[%s] reg 0x%02x read failed: %s", label, regs[i], esp_err_to_name(e));
        }
    }
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

static uint16_t read_le16(const uint8_t *src)
{
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8U);
}

static uint32_t read_le32(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8U) |
           ((uint32_t)src[2] << 16U) | ((uint32_t)src[3] << 24U);
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

static i2s_std_slot_config_t build_i2s_rx_slot_config(const audio_input_i2s_config_t *cfg)
{
    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT,
        cfg->channels == 1U ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO);

    if (cfg->channels == 1U) {
        /* On ESP32-S3 standard-mode RX the helper macro defaults mono capture
         * to BOTH slots. That yields interleaved L/R words even for a single
         * mic, so a mono WAV header makes playback sound half-speed. */
        slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    } else {
        slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    }

    return slot_cfg;
}

static i2s_std_slot_config_t build_i2s_tx_slot_config(uint16_t channels)
{
    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT,
        channels == 1U ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO);

    slot_cfg.slot_mask = channels == 1U ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_BOTH;
    return slot_cfg;
}

static size_t duplicate_mono_pcm16_to_stereo(const uint8_t *src,
                                             size_t src_bytes,
                                             uint8_t *dst,
                                             size_t dst_size)
{
    size_t samples = src_bytes / 2U;

    if (dst_size / 4U < samples) {
        samples = dst_size / 4U;
    }

    for (size_t i = 0; i < samples; ++i) {
        uint16_t sample = read_le16(src + (i * 2U));
        write_le16(dst + (i * 4U), sample);
        write_le16(dst + (i * 4U) + 2U, sample);
    }

    return samples * 4U;
}

typedef struct {
    const uint8_t *pcm;
    size_t pcm_size;
    uint32_t sample_rate_hz;
    uint16_t channels;
    uint16_t bits_per_sample;
} wav_view_t;

static esp_err_t parse_pcm_wav(const uint8_t *wav, size_t wav_size, wav_view_t *out)
{
    size_t offset = 12U;
    bool have_fmt = false;
    bool have_data = false;
    wav_view_t parsed = {0};

    if (wav == NULL || out == NULL || wav_size < 44U ||
        memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8U, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    while (offset + 8U <= wav_size) {
        const uint8_t *chunk = wav + offset;
        uint32_t chunk_size = read_le32(chunk + 4U);
        size_t chunk_data = offset + 8U;

        if (chunk_data > wav_size || chunk_size > wav_size - chunk_data) {
            return ESP_ERR_INVALID_SIZE;
        }

        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (chunk_size < 16U) {
                return ESP_ERR_INVALID_SIZE;
            }
            uint16_t audio_format = read_le16(wav + chunk_data);
            parsed.channels = read_le16(wav + chunk_data + 2U);
            parsed.sample_rate_hz = read_le32(wav + chunk_data + 4U);
            parsed.bits_per_sample = read_le16(wav + chunk_data + 14U);
            if (audio_format != 1U || parsed.sample_rate_hz == 0U ||
                (parsed.channels != 1U && parsed.channels != 2U) ||
                parsed.bits_per_sample != 16U) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            have_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            parsed.pcm = wav + chunk_data;
            parsed.pcm_size = chunk_size;
            have_data = chunk_size > 0U;
        }

        offset = chunk_data + chunk_size + (chunk_size & 1U);
    }

    if (!have_fmt || !have_data) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = parsed;
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
        /* Another module (e.g. touch_input) may have already created this I2C
         * port. Reuse it instead of registering a duplicate bus. */
        esp_err_t get_err = i2c_master_get_bus_handle(cfg->i2c_port, &s_i2c_bus_handle);
        if (get_err != ESP_OK || s_i2c_bus_handle == NULL) {
            s_i2c_bus_handle = NULL;
            bus_cfg.i2c_port = cfg->i2c_port;
            bus_cfg.sda_io_num = cfg->i2c_sda_gpio;
            bus_cfg.scl_io_num = cfg->i2c_scl_gpio;
            bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
            bus_cfg.glitch_ignore_cnt = 7;
            bus_cfg.flags.enable_internal_pullup = true;
            ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus_handle), TAG, "i2c bus init failed");
        } else {
            ESP_LOGI(TAG, "reusing existing I2C bus on port %d", cfg->i2c_port);
        }
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
    es8311_dump_registers("post-init");
    /* NOTE: don't call es8311_start_adc() here. The ADC needs a live MCLK to
     * produce valid samples; MCLK only starts once the I2S channel is enabled.
     * Callers start the ADC explicitly after i2s_channel_enable() succeeds. */

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

/* ------------------------------------------------------------------ */
/* Streaming press-and-hold recording                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    audio_input_i2s_config_t cfg;
    i2s_chan_handle_t rx_handle;
    uint8_t *pcm_buf;        /* allocated in PSRAM when available */
    size_t capacity;          /* bytes */
    size_t captured;          /* bytes */
    volatile bool stop_requested;
    SemaphoreHandle_t done_sem;
} audio_recording_ctx_t;

static audio_recording_ctx_t *s_rec_ctx;
static bool s_rec_stop_in_progress;
static bool s_playback_active;
static portMUX_TYPE s_rec_lock = portMUX_INITIALIZER_UNLOCKED;

static void audio_recording_task(void *arg)
{
    audio_recording_ctx_t *ctx = (audio_recording_ctx_t *)arg;

    ESP_LOGI(TAG, "recording task started capacity=%u bytes", (unsigned)ctx->capacity);
    /* Read a bounded chunk per iteration so we stay responsive to stop_requested.
     * 4 KB at 16 kHz / mono / 16-bit is ~128 ms of audio. */
    const size_t chunk_bytes = 4096U;
    while (!ctx->stop_requested && ctx->captured < ctx->capacity) {
        size_t to_read = ctx->capacity - ctx->captured;
        if (to_read > chunk_bytes) {
            to_read = chunk_bytes;
        }
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(ctx->rx_handle,
                                         ctx->pcm_buf + ctx->captured,
                                         to_read,
                                         &bytes_read,
                                         pdMS_TO_TICKS(500));
        /* On timeout the driver still reports the partial bytes it managed to
         * deliver — count them, don't discard them. */
        if (ret == ESP_OK || ret == ESP_ERR_TIMEOUT) {
            ctx->captured += bytes_read;
        } else {
            ESP_LOGE(TAG, "recording read failed: %s", esp_err_to_name(ret));
            break;
        }
    }
    if (ctx->captured >= ctx->capacity) {
        ESP_LOGW(TAG, "recording hit max-duration cap (%u bytes)", (unsigned)ctx->capacity);
    }
    xSemaphoreGive(ctx->done_sem);
    vTaskDelete(NULL);
}

esp_err_t audio_input_recording_start(const audio_input_i2s_config_t *cfg,
                                      uint32_t max_duration_ms)
{
    if (cfg == NULL || max_duration_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->bits_per_sample != 16U || (cfg->channels != 1U && cfg->channels != 2U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->bclk_gpio < 0 || cfg->ws_gpio < 0 || cfg->din_gpio < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_rec_lock);
    bool already_recording = s_rec_ctx != NULL || s_rec_stop_in_progress || s_playback_active;
    portEXIT_CRITICAL(&s_rec_lock);
    if (already_recording) {
        ESP_LOGW(TAG, "recording_start called while audio pipeline is busy");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(audio_input_prepare_i2s_capture(cfg), TAG, "prepare codec failed");

    size_t capacity = audio_input_pcm_size(
        cfg->sample_rate_hz, cfg->channels, cfg->bits_per_sample, max_duration_ms);
    if (capacity == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }

    audio_recording_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ctx->pcm_buf = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM);
    if (ctx->pcm_buf == NULL) {
        ctx->pcm_buf = heap_caps_malloc(capacity, MALLOC_CAP_8BIT);
    }
    if (ctx->pcm_buf == NULL) {
        free(ctx);
        return ESP_ERR_NO_MEM;
    }
    ctx->cfg = *cfg;
    ctx->capacity = capacity;
    ctx->done_sem = xSemaphoreCreateBinary();
    if (ctx->done_sem == NULL) {
        free(ctx->pcm_buf);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG((i2s_port_t)cfg->i2s_port,
                                                            I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &ctx->rx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        goto fail;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(cfg->sample_rate_hz),
        .slot_cfg = build_i2s_rx_slot_config(cfg),
        .gpio_cfg = {
            .mclk = cfg->mclk_gpio,
            .bclk = cfg->bclk_gpio,
            .ws = cfg->ws_gpio,
            .dout = I2S_GPIO_UNUSED,
            .din = cfg->din_gpio,
            .invert_flags = {0},
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    err = i2s_channel_init_std_mode(ctx->rx_handle, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        goto fail;
    }
    err = i2s_channel_enable(ctx->rx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        goto fail;
    }

    /* MCLK is live now — let it stabilize, then start the codec ADC. */
    vTaskDelay(pdMS_TO_TICKS(20));
    err = es8311_start_adc();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "es8311_start_adc failed: %s", esp_err_to_name(err));
        i2s_channel_disable(ctx->rx_handle);
        goto fail;
    }
    /* Discard the first ~50 ms of audio; the ADC anti-pop ramp produces a
     * thump otherwise. The reader task will drop the leading DMA buffer. */
    vTaskDelay(pdMS_TO_TICKS(50));
    es8311_dump_registers("post-adc");

    BaseType_t task_ok = xTaskCreate(
        audio_recording_task, "audio_rec", 4096, ctx, 6, NULL);
    if (task_ok != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    portENTER_CRITICAL(&s_rec_lock);
    s_rec_ctx = ctx;
    portEXIT_CRITICAL(&s_rec_lock);
    ESP_LOGI(TAG,
             "recording started max_ms=%" PRIu32 " capacity=%u bytes",
             max_duration_ms,
             (unsigned)capacity);
    return ESP_OK;

fail:
    if (ctx->rx_handle != NULL) {
        i2s_channel_disable(ctx->rx_handle);
        i2s_del_channel(ctx->rx_handle);
    }
    if (ctx->done_sem != NULL) {
        vSemaphoreDelete(ctx->done_sem);
    }
    free(ctx->pcm_buf);
    free(ctx);
    return err;
}

bool audio_input_recording_is_active(void)
{
    portENTER_CRITICAL(&s_rec_lock);
    bool active = s_rec_ctx != NULL;
    portEXIT_CRITICAL(&s_rec_lock);
    return active;
}

bool audio_input_playback_is_active(void)
{
    portENTER_CRITICAL(&s_rec_lock);
    bool active = s_playback_active;
    portEXIT_CRITICAL(&s_rec_lock);
    return active;
}

esp_err_t audio_input_recording_stop(uint8_t **wav_out,
                                     size_t *wav_size_out,
                                     uint32_t *duration_ms_out)
{
    audio_recording_ctx_t *ctx;

    portENTER_CRITICAL(&s_rec_lock);
    ctx = s_rec_ctx;
    if (ctx == NULL) {
        portEXIT_CRITICAL(&s_rec_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_rec_stop_in_progress) {
        portEXIT_CRITICAL(&s_rec_lock);
        ESP_LOGW(TAG, "recording_stop called while stop already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    s_rec_stop_in_progress = true;
    portEXIT_CRITICAL(&s_rec_lock);

    ctx->stop_requested = true;
    /* Worst case the read in flight has a 100ms timeout. Wait up to 2s. */
    if (xSemaphoreTake(ctx->done_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "recording task did not finish in 2s; cleaning up anyway");
    }

    i2s_channel_disable(ctx->rx_handle);
    i2s_del_channel(ctx->rx_handle);
    vSemaphoreDelete(ctx->done_sem);
    portENTER_CRITICAL(&s_rec_lock);
    s_rec_ctx = NULL;
    s_rec_stop_in_progress = false;
    portEXIT_CRITICAL(&s_rec_lock);

    size_t pcm_size = ctx->captured;
    uint32_t duration_ms = 0;
    if (ctx->cfg.sample_rate_hz != 0U && ctx->cfg.channels != 0U) {
        uint64_t bytes_per_second =
            (uint64_t)ctx->cfg.sample_rate_hz * ctx->cfg.channels *
            (ctx->cfg.bits_per_sample / 8U);
        if (bytes_per_second > 0U) {
            duration_ms = (uint32_t)(((uint64_t)pcm_size * 1000ULL) / bytes_per_second);
        }
    }
    if (duration_ms_out != NULL) {
        *duration_ms_out = duration_ms;
    }

    ESP_LOGI(TAG,
             "recording stopped pcm_bytes=%u duration_ms=%" PRIu32,
             (unsigned)pcm_size,
             duration_ms);

    esp_err_t result = ESP_OK;

    if (wav_out == NULL || wav_size_out == NULL) {
        /* Caller wants to discard the take. */
        free(ctx->pcm_buf);
        free(ctx);
        return ESP_OK;
    }

    if (pcm_size == 0U) {
        free(ctx->pcm_buf);
        free(ctx);
        *wav_out = NULL;
        *wav_size_out = 0;
        return ESP_ERR_INVALID_SIZE;
    }

    size_t wav_size = 44U + pcm_size;
    uint8_t *wav_buf = heap_caps_malloc(wav_size, MALLOC_CAP_SPIRAM);
    if (wav_buf == NULL) {
        wav_buf = malloc(wav_size);
    }
    if (wav_buf == NULL) {
        result = ESP_ERR_NO_MEM;
    } else {
        result = write_wav_header(wav_buf,
                                  wav_size,
                                  ctx->cfg.sample_rate_hz,
                                  ctx->cfg.channels,
                                  ctx->cfg.bits_per_sample,
                                  pcm_size);
        if (result == ESP_OK) {
            memcpy(wav_buf + 44U, ctx->pcm_buf, pcm_size);
            *wav_out = wav_buf;
            *wav_size_out = wav_size;
        } else {
            free(wav_buf);
        }
    }

    free(ctx->pcm_buf);
    free(ctx);
    return result;
}

esp_err_t audio_input_play_wav(const audio_input_i2s_config_t *cfg,
                               const uint8_t *wav,
                               size_t wav_size)
{
    wav_view_t view = {0};
    audio_input_i2s_config_t play_cfg;
    i2s_chan_handle_t tx_handle = NULL;
    uint8_t *stereo_chunk = NULL;
    bool tx_enabled = false;
    bool pa_enabled = false;
    esp_err_t err;

    if (cfg == NULL || wav == NULL || wav_size == 0U || cfg->dout_gpio < 0 ||
        cfg->bclk_gpio < 0 || cfg->ws_gpio < 0 || cfg->bits_per_sample != 16U) {
        return ESP_ERR_INVALID_ARG;
    }

    err = parse_pcm_wav(wav, wav_size, &view);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WAV parse failed: %s", esp_err_to_name(err));
        return err;
    }
    if (view.sample_rate_hz != cfg->sample_rate_hz) {
        ESP_LOGW(TAG,
                 "unsupported playback sample rate wav=%" PRIu32 " configured=%" PRIu32,
                 view.sample_rate_hz,
                 cfg->sample_rate_hz);
        return ESP_ERR_NOT_SUPPORTED;
    }

    portENTER_CRITICAL(&s_rec_lock);
    bool busy = s_rec_ctx != NULL || s_rec_stop_in_progress || s_playback_active;
    if (!busy) {
        s_playback_active = true;
    }
    portEXIT_CRITICAL(&s_rec_lock);
    if (busy) {
        ESP_LOGW(TAG, "playback requested while audio pipeline is busy");
        return ESP_ERR_INVALID_STATE;
    }

    play_cfg = *cfg;
    play_cfg.channels = view.channels == 1U ? 2U : view.channels;
    play_cfg.bits_per_sample = view.bits_per_sample;
    play_cfg.duration_ms = 0;

    err = audio_input_prepare_i2s_capture(&play_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "prepare codec for playback failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG((i2s_port_t)play_cfg.i2s_port,
                                                            I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    err = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s tx channel create failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(play_cfg.sample_rate_hz),
        .slot_cfg = build_i2s_tx_slot_config(play_cfg.channels),
        .gpio_cfg = {
            .mclk = play_cfg.mclk_gpio,
            .bclk = play_cfg.bclk_gpio,
            .ws = play_cfg.ws_gpio,
            .dout = play_cfg.dout_gpio,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {0},
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    err = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s tx init failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = i2s_channel_enable(tx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s tx enable failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    tx_enabled = true;

    vTaskDelay(pdMS_TO_TICKS(20));
    err = es8311_start_dac(play_cfg.playback_volume_percent, play_cfg.playback_muted);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "es8311_start_dac failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    /* On the Waveshare ESP32-S3-Touch-ePaper-1.54 V2 board the two "PA" pins
     * have OPPOSITE active polarities, which is easy to get wrong:
     *   - pa_enable_gpio (GPIO42, Audio_PWR_PIN) gates the entire audio
     *     power rail and is ACTIVE LOW. Driving it HIGH cuts power to the
     *     ES8311 and the speaker amplifier, so the codec produces no sound
     *     even though I2S clocks and DAC registers look correct.
     *   - pa_control_gpio (GPIO46, PA_CTRL) is the speaker amplifier enable
     *     and is ACTIVE HIGH. */
    if (play_cfg.pa_enable_gpio >= 0) {
        err = configure_output_pin(play_cfg.pa_enable_gpio, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "pa_enable failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
        pa_enabled = true;
    }
    if (play_cfg.pa_control_gpio >= 0) {
        err = configure_output_pin(play_cfg.pa_control_gpio, 1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "pa_control failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
        pa_enabled = true;
    }
    if (pa_enabled) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGI(TAG,
             "playback started pcm_bytes=%u sample_rate=%" PRIu32
             " wav_channels=%u i2s_channels=%u volume=%u%% muted=%d",
             (unsigned)view.pcm_size,
             view.sample_rate_hz,
             (unsigned)view.channels,
             (unsigned)play_cfg.channels,
             (unsigned)play_cfg.playback_volume_percent,
             play_cfg.playback_muted);

    size_t offset = 0;
    if (view.channels == 1U) {
        stereo_chunk = malloc(4096U);
        if (stereo_chunk == NULL) {
            err = ESP_ERR_NO_MEM;
            ESP_LOGE(TAG, "allocate stereo playback chunk failed");
            goto cleanup;
        }
    }
    while (offset < view.pcm_size) {
        size_t to_write = view.pcm_size - offset;
        if (to_write > 4096U) {
            to_write = 4096U;
        }
        if (view.channels == 1U && to_write > 2048U) {
            to_write = 2048U;
        }
        to_write &= ~(size_t)1U;
        if (to_write == 0U) {
            break;
        }

        const uint8_t *write_buf = view.pcm + offset;
        size_t write_len = to_write;
        if (view.channels == 1U) {
            write_len = duplicate_mono_pcm16_to_stereo(write_buf, to_write, stereo_chunk, 4096U);
            write_buf = stereo_chunk;
        }

        size_t written = 0;
        err = i2s_channel_write(tx_handle,
                                write_buf,
                                write_len,
                                &written,
                                pdMS_TO_TICKS(1000));
        if ((err != ESP_OK && err != ESP_ERR_TIMEOUT) || written == 0U) {
            ESP_LOGE(TAG, "i2s tx write failed: %s", esp_err_to_name(err));
            goto cleanup;
        }
        offset += view.channels == 1U ? (written / 2U) : written;
    }

    ESP_LOGI(TAG, "playback finished pcm_bytes=%u", (unsigned)view.pcm_size);
    err = ESP_OK;

cleanup:
    if (pa_enabled) {
        /* Disable the speaker amplifier (active HIGH) but keep the audio
         * power rail asserted (active LOW) so a subsequent recording can
         * reuse the already-initialized codec without re-powering it. */
        if (play_cfg.pa_control_gpio >= 0) {
            (void)gpio_set_level(play_cfg.pa_control_gpio, 0);
        }
        if (play_cfg.pa_enable_gpio >= 0) {
            (void)gpio_set_level(play_cfg.pa_enable_gpio, 0);
        }
    }
    if (tx_handle != NULL) {
        if (tx_enabled) {
            (void)i2s_channel_disable(tx_handle);
        }
        (void)i2s_del_channel(tx_handle);
    }
    free(stereo_chunk);
    portENTER_CRITICAL(&s_rec_lock);
    s_playback_active = false;
    portEXIT_CRITICAL(&s_rec_lock);
    return err;
}
