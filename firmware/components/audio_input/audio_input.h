#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t sample_rate_hz;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t duration_ms;
    float tone_frequency_hz;
    int16_t amplitude;
} audio_input_demo_config_t;

size_t audio_input_demo_pcm_size(const audio_input_demo_config_t *cfg);
size_t audio_input_demo_wav_size(const audio_input_demo_config_t *cfg);

esp_err_t audio_input_generate_demo_wav(const audio_input_demo_config_t *cfg,
                                        uint8_t *out_buf,
                                        size_t out_len,
                                        size_t *written_out);

typedef struct {
    int i2s_port;
    int i2c_port;
    int i2c_sda_gpio;
    int i2c_scl_gpio;
    int codec_i2c_addr;
    int pa_enable_gpio;
    int pa_control_gpio;
    int mclk_gpio;
    int bclk_gpio;
    int ws_gpio;
    int din_gpio;
    uint32_t sample_rate_hz;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t duration_ms;
} audio_input_i2s_config_t;

size_t audio_input_pcm_size(uint32_t sample_rate_hz,
                            uint16_t channels,
                            uint16_t bits_per_sample,
                            uint32_t duration_ms);
size_t audio_input_wav_size(uint32_t sample_rate_hz,
                            uint16_t channels,
                            uint16_t bits_per_sample,
                            uint32_t duration_ms);

esp_err_t audio_input_prepare_i2s_capture(const audio_input_i2s_config_t *cfg);

esp_err_t audio_input_capture_i2s_wav(const audio_input_i2s_config_t *cfg,
                                      uint8_t *out_buf,
                                      size_t out_len,
                                      size_t *written_out);

/*
 * Press-and-hold recording API.
 *
 * audio_input_recording_start() initializes the codec/I2S channel, allocates
 * a PCM buffer big enough for max_duration_ms, and spawns a background task
 * that keeps reading samples until either the buffer fills or
 * audio_input_recording_stop() is called.
 *
 * audio_input_recording_stop() signals the reader to finish, builds a WAV
 * file in a freshly-allocated buffer, and hands ownership to the caller via
 * *wav_out (caller must free()). Pass wav_out == NULL to discard the take.
 */
esp_err_t audio_input_recording_start(const audio_input_i2s_config_t *cfg,
                                      uint32_t max_duration_ms);
bool audio_input_recording_is_active(void);
esp_err_t audio_input_recording_stop(uint8_t **wav_out,
                                     size_t *wav_size_out,
                                     uint32_t *duration_ms_out);
