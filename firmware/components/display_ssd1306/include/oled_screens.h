/*
 * oled_screens - turns a ui_render_t into pixels.
 *
 * The ui state machine decides WHAT to show; this decides HOW. It holds
 * no state of its own: the same render always produces the same frame,
 * which is what makes it testable and what lets the driver skip frames
 * that did not change.
 *
 * Strings are ASCII-only because the font is (no "RELÓGIO"). They live
 * in one table in oled_screens.c so the language can be swapped for
 * foreign guests without touching layout code.
 */
#ifndef OLED_SCREENS_H
#define OLED_SCREENS_H

#include "oled_fb.h"
#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Geometry of the progress bar, exposed so the tests can measure it. */
#define OLED_BAR_X 4
#define OLED_BAR_W 120
#define OLED_BAR_H 10
#define OLED_BAR_INNER_W (OLED_BAR_W - 4) /* outline plus one pixel gap */

void oled_compose(oled_fb_t *fb, const ui_render_t *r);

/* "05:00". Clamps at 99:59 rather than overflowing the buffer. */
void oled_fmt_mmss(char out[6], uint32_t seconds);

#ifdef __cplusplus
}
#endif

#endif /* OLED_SCREENS_H */
