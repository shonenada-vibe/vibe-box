#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TOUCH_EVENT_DOWN = 0,
    TOUCH_EVENT_UP   = 1,
    TOUCH_EVENT_SWIPE_LEFT,
    TOUCH_EVENT_SWIPE_RIGHT,
} touch_event_t;

typedef void (*touch_event_cb_t)(touch_event_t event, void *user_ctx);

typedef struct {
    int      i2c_port;        /* shared with audio codec / sensors */
    int      i2c_sda_gpio;
    int      i2c_scl_gpio;
    int      reset_gpio;      /* CST816T RST, active-low; -1 to skip */
    int      int_gpio;        /* CST816T INT (currently informational; -1 to skip) */
    uint8_t  i2c_addr;        /* 7-bit, default 0x15 */
    uint32_t i2c_speed_hz;    /* default 100000 if 0 */
    uint32_t poll_period_ms;  /* default 20 if 0 */
    uint8_t  debounce_samples;/* default 2 if 0 */
} touch_input_config_t;

/*
 * Initializes the CST816T capacitive touch controller and starts a background
 * polling task that emits TOUCH_EVENT_DOWN / TOUCH_EVENT_UP edges and simple
 * horizontal swipe events through cb.
 *
 * The callback runs in the touch task context. Keep it short or hand off to
 * another queue/task. cb must remain valid for the lifetime of the process.
 */
esp_err_t touch_input_init(const touch_input_config_t *cfg,
                           touch_event_cb_t cb,
                           void *user_ctx);

/* Returns true if a finger is currently considered pressed (post-debounce). */
bool touch_input_is_pressed(void);

#ifdef __cplusplus
}
#endif
