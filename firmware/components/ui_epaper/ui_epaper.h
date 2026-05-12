#ifndef UI_EPAPER_H
#define UI_EPAPER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_EPAPER_WIDTH       200
#define UI_EPAPER_HEIGHT      200

/* Maximum lines that show_lines() will render. */
#define UI_EPAPER_MAX_LINES   16
/* Maximum characters a single rendered line can hold. */
#define UI_EPAPER_MAX_COLS    32

/**
 * Initialize the on-board e-paper display.
 *
 * Powers the panel rail, configures GPIO and SPI, performs a full reset and
 * leaves the panel in partial-refresh mode so subsequent flushes are fast.
 *
 * Safe to call once during startup. Subsequent calls are ignored.
 */
esp_err_t ui_epaper_init(void);

/* Reports whether ui_epaper_init() succeeded. */
bool ui_epaper_is_ready(void);

/* Clear the in-RAM frame buffer to white. Does not push to the panel. */
void ui_epaper_clear(void);

/* Draw an ASCII string at pixel (x, y) using the bundled 8x8 font. */
void ui_epaper_draw_text(int x, int y, const char *text);

/* Draw a horizontal line of black pixels at row y from x0 to x1 inclusive. */
void ui_epaper_draw_hline(int y, int x0, int x1);

/* Push the current buffer to the panel using a partial refresh. */
esp_err_t ui_epaper_flush(void);

/*
 * High-level helper: render a status page made of a headline and a detail
 * string. Long detail strings are wrapped to the screen width.
 */
esp_err_t ui_epaper_show_status(const char *headline, const char *detail);

/*
 * High-level helper: render a result page with a headline and a list of
 * already-prepared display lines.
 */
esp_err_t ui_epaper_show_lines(const char *headline,
                               const char *const *lines,
                               size_t line_count);

#ifdef __cplusplus
}
#endif

#endif /* UI_EPAPER_H */
