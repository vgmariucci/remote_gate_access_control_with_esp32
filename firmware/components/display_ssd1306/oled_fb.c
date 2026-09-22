#include "oled_fb.h"

#include <string.h>

/* Classic 5x7 font, 0x20..0x5A. Column-major, bit 0 = top row. */
static const uint8_t FONT5X7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* ' ' */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* '!' */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* '"' */
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* '#' */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* '$' */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* '%' */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* '&' */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* ''' */
    {0x00, 0x1C, 0x22, 0x41, 0x00}, /* '(' */
    {0x00, 0x41, 0x22, 0x1C, 0x00}, /* ')' */
    {0x14, 0x08, 0x3E, 0x08, 0x14}, /* '*' */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* '+' */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* ',' */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* '-' */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* '.' */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* '/' */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* '0' */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* '1' */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* '2' */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* '3' */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* '4' */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* '5' */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* '6' */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* '7' */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* '8' */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* '9' */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* ':' */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* ';' */
    {0x08, 0x14, 0x22, 0x41, 0x00}, /* '<' */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* '=' */
    {0x00, 0x41, 0x22, 0x14, 0x08}, /* '>' */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* '?' */
    {0x32, 0x49, 0x79, 0x41, 0x3E}, /* '@' */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* 'A' */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* 'B' */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* 'C' */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* 'D' */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* 'E' */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* 'F' */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* 'G' */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* 'H' */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* 'I' */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* 'J' */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* 'K' */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* 'L' */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* 'M' */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* 'N' */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* 'O' */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* 'P' */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* 'Q' */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* 'R' */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* 'S' */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* 'T' */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* 'U' */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* 'V' */
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* 'W' */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* 'X' */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* 'Y' */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* 'Z' */
};

#define FONT_FIRST 0x20
#define FONT_LAST 0x5A
#define GLYPH_W 5
#define GLYPH_H 7
#define ADVANCE 6 /* glyph + one column of spacing */

void oled_fb_clear(oled_fb_t *fb)
{
    if (fb != NULL) {
        memset(fb->buf, 0, sizeof(fb->buf));
    }
}

void oled_fb_pixel(oled_fb_t *fb, int x, int y, bool on)
{
    if (fb == NULL || x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) {
        return;
    }
    size_t i = (size_t)(y / 8) * OLED_W + (size_t)x;
    uint8_t mask = (uint8_t)(1u << (y % 8));
    if (on) {
        fb->buf[i] |= mask;
    } else {
        fb->buf[i] &= (uint8_t)~mask;
    }
}

bool oled_fb_get(const oled_fb_t *fb, int x, int y)
{
    if (fb == NULL || x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) {
        return false;
    }
    return (fb->buf[(size_t)(y / 8) * OLED_W + (size_t)x] >> (y % 8)) & 1u;
}

void oled_fb_fill_rect(oled_fb_t *fb, int x, int y, int w, int h, bool on)
{
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            oled_fb_pixel(fb, xx, yy, on);
        }
    }
}

void oled_fb_rect(oled_fb_t *fb, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    oled_fb_fill_rect(fb, x, y, w, 1, true);
    oled_fb_fill_rect(fb, x, y + h - 1, w, 1, true);
    oled_fb_fill_rect(fb, x, y, 1, h, true);
    oled_fb_fill_rect(fb, x + w - 1, y, 1, h, true);
}

static const uint8_t *glyph_for(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    if ((unsigned char)c < FONT_FIRST || (unsigned char)c > FONT_LAST) {
        return FONT5X7[0]; /* blank */
    }
    return FONT5X7[(unsigned char)c - FONT_FIRST];
}

static void draw_char(oled_fb_t *fb, int x, int y, char c, int scale)
{
    const uint8_t *g = glyph_for(c);
    for (int col = 0; col < GLYPH_W; col++) {
        for (int row = 0; row < GLYPH_H; row++) {
            if ((g[col] >> row) & 1u) {
                oled_fb_fill_rect(fb, x + col * scale, y + row * scale, scale, scale, true);
            }
        }
    }
}

void oled_fb_text(oled_fb_t *fb, int x, int y, const char *s, int scale)
{
    if (fb == NULL || s == NULL || scale < 1) {
        return;
    }
    for (; *s != '\0'; s++) {
        draw_char(fb, x, y, *s, scale);
        x += ADVANCE * scale;
    }
}

int oled_fb_text_width(const char *s, int scale)
{
    if (s == NULL || scale < 1) {
        return 0;
    }
    int n = (int)strlen(s);
    /* Last glyph needs no trailing spacing column. */
    return n == 0 ? 0 : (n * ADVANCE - 1) * scale;
}

void oled_fb_text_centered(oled_fb_t *fb, int y, const char *s, int scale)
{
    int w = oled_fb_text_width(s, scale);
    oled_fb_text(fb, (OLED_W - w) / 2, y, s, scale);
}

void oled_fb_border(oled_fb_t *fb)
{
    oled_fb_rect(fb, 0, 0, OLED_W, OLED_H);
}

uint8_t oled_fb_dirty_pages(const oled_fb_t *prev, const oled_fb_t *next)
{
    if (prev == NULL || next == NULL) {
        return 0xFF;
    }
    uint8_t mask = 0;
    for (int p = 0; p < OLED_PAGES; p++) {
        if (memcmp(&prev->buf[p * OLED_W], &next->buf[p * OLED_W], OLED_W) != 0) {
            mask |= (uint8_t)(1u << p);
        }
    }
    return mask;
}
