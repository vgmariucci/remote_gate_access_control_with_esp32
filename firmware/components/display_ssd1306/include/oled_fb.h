/*
 * oled_fb - 128x64 monochrome framebuffer, drawing primitives and a
 * 5x7 font.
 *
 * Pure C99. The memory layout is the SSD1306's native page format, so
 * the driver can stream a page straight out of this buffer with no
 * conversion: 8 pages of 128 columns, one byte per column per page,
 * bit 0 at the top of the page.
 *
 *     byte index = page * 128 + x      where page = y / 8
 *     bit        = y % 8
 */
#ifndef OLED_FB_H
#define OLED_FB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OLED_W 128
#define OLED_H 64
#define OLED_PAGES (OLED_H / 8)

typedef struct {
    uint8_t buf[OLED_W * OLED_PAGES];
} oled_fb_t;

void oled_fb_clear(oled_fb_t *fb);

/* Out-of-bounds coordinates are ignored, never wrapped. */
void oled_fb_pixel(oled_fb_t *fb, int x, int y, bool on);
bool oled_fb_get(const oled_fb_t *fb, int x, int y);

void oled_fb_fill_rect(oled_fb_t *fb, int x, int y, int w, int h, bool on);
void oled_fb_rect(oled_fb_t *fb, int x, int y, int w, int h);

/* Uppercase ASCII, digits and common punctuation. Lowercase is folded
 * to uppercase. Anything else draws as a blank cell, so the font has
 * no accented characters: "RELOGIO", not "RELÓGIO". */
void oled_fb_text(oled_fb_t *fb, int x, int y, const char *s, int scale);
int oled_fb_text_width(const char *s, int scale);
void oled_fb_text_centered(oled_fb_t *fb, int y, const char *s, int scale);

/* One-pixel frame on all four edges. Flash this once on bring-up: if
 * all four edges are crisp the column offset is right; if the frame is
 * shifted two pixels with noise along one side, the controller is an
 * SH1106 and the offset needs to be 2. */
void oled_fb_border(oled_fb_t *fb);

/* Bit n set when page n differs between the two frames. Lets the
 * driver resend only the pages that changed: the countdown bar lives
 * in one page, so a bar tick costs 129 bytes rather than 1032. */
uint8_t oled_fb_dirty_pages(const oled_fb_t *prev, const oled_fb_t *next);

#ifdef __cplusplus
}
#endif

#endif /* OLED_FB_H */
