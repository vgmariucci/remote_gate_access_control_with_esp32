/*
 * Unity tests for the framebuffer and screen composition.
 *
 * The one that matters most is "the entry screen never reveals the
 * entry length": it checks the property in pixels, which is where a
 * bystander would actually read it.
 */
#include <stdlib.h>
#include <string.h>

#include "oled_screens.h"
#include "unity.h"

static oled_fb_t fb, fb2;

static int lit_in_rect(const oled_fb_t *f, int x, int y, int w, int h)
{
    int n = 0;
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            n += oled_fb_get(f, xx, yy) ? 1 : 0;
        }
    }
    return n;
}

static int lit_total(const oled_fb_t *f)
{
    return lit_in_rect(f, 0, 0, OLED_W, OLED_H);
}

static ui_render_t render(ui_screen_t s)
{
    ui_render_t r;
    memset(&r, 0, sizeof(r));
    r.screen = s;
    r.attempts_max = 5;
    return r;
}

TEST_CASE("pixels land in the controller's native page layout", "[oled]")
{
    oled_fb_clear(&fb);
    oled_fb_pixel(&fb, 0, 0, true);
    oled_fb_pixel(&fb, 0, 7, true);
    oled_fb_pixel(&fb, 0, 8, true);
    oled_fb_pixel(&fb, 127, 63, true);

    TEST_ASSERT_EQUAL_HEX8(0x81, fb.buf[0]);   /* bits 0 and 7, page 0 */
    TEST_ASSERT_EQUAL_HEX8(0x01, fb.buf[128]); /* bit 0, page 1 */
    TEST_ASSERT_EQUAL_HEX8(0x80, fb.buf[1023]);
}

TEST_CASE("out-of-bounds pixels are ignored, not wrapped", "[oled]")
{
    oled_fb_clear(&fb);
    oled_fb_pixel(&fb, -1, 0, true);
    oled_fb_pixel(&fb, 128, 0, true);
    oled_fb_pixel(&fb, 0, 64, true);
    oled_fb_pixel(&fb, 0, -1, true);
    TEST_ASSERT_EQUAL_INT(0, lit_total(&fb));
}

TEST_CASE("idle composes a fully dark frame", "[oled]")
{
    ui_render_t r = render(UI_SCREEN_IDLE);
    oled_compose(&fb, &r);
    TEST_ASSERT_EQUAL_INT(0, lit_total(&fb));
}

TEST_CASE("the border lights all four edges and nothing inside", "[oled]")
{
    oled_fb_clear(&fb);
    oled_fb_border(&fb);
    TEST_ASSERT_TRUE(oled_fb_get(&fb, 0, 0));
    TEST_ASSERT_TRUE(oled_fb_get(&fb, 127, 0));
    TEST_ASSERT_TRUE(oled_fb_get(&fb, 0, 63));
    TEST_ASSERT_TRUE(oled_fb_get(&fb, 127, 63));
    TEST_ASSERT_EQUAL_INT(0, lit_in_rect(&fb, 1, 1, OLED_W - 2, OLED_H - 2));
}

TEST_CASE("the entry screen never reveals the entry length", "[oled]")
{
    ui_render_t r = render(UI_SCREEN_ENTRY);
    r.progress_permille = 700;
    oled_compose(&fb, &r);

    /* Everything lit must be inside the bar's rectangle: no dots, no
     * digits, no cursor anywhere else on the screen. */
    int bar_y = (OLED_H - OLED_BAR_H) / 2;
    int inside = lit_in_rect(&fb, OLED_BAR_X, bar_y, OLED_BAR_W, OLED_BAR_H);
    TEST_ASSERT_TRUE(inside > 0);
    TEST_ASSERT_EQUAL_INT(inside, lit_total(&fb));
}

TEST_CASE("the countdown bar width is proportional to progress", "[oled]")
{
    int bar_y = (OLED_H - OLED_BAR_H) / 2;
    int mid = bar_y + OLED_BAR_H / 2;
    const uint16_t cases[] = {1000, 500, 0};
    const int expect[] = {OLED_BAR_INNER_W, OLED_BAR_INNER_W / 2, 0};

    for (int i = 0; i < 3; i++) {
        ui_render_t r = render(UI_SCREEN_ENTRY);
        r.progress_permille = cases[i];
        oled_compose(&fb, &r);
        TEST_ASSERT_EQUAL_INT(expect[i],
                              lit_in_rect(&fb, OLED_BAR_X + 2, mid, OLED_BAR_INNER_W, 1));
    }
}

TEST_CASE("granted draws a large centred OK", "[oled]")
{
    ui_render_t r = render(UI_SCREEN_GRANTED);
    oled_compose(&fb, &r);
    TEST_ASSERT_TRUE(lit_total(&fb) > 300);

    int left = -1, right = -1;
    for (int x = 0; x < OLED_W && left < 0; x++) {
        if (lit_in_rect(&fb, x, 0, 1, OLED_H) > 0) {
            left = x;
        }
    }
    for (int x = OLED_W - 1; x >= 0 && right < 0; x--) {
        if (lit_in_rect(&fb, x, 0, 1, OLED_H) > 0) {
            right = x;
        }
    }
    /* Symmetric to within a pixel of integer rounding. */
    TEST_ASSERT_TRUE(abs(left - (OLED_W - 1 - right)) <= 1);
}

TEST_CASE("the denied screen reflects the attempt count", "[oled]")
{
    ui_render_t r = render(UI_SCREEN_DENIED);
    r.attempts_used = 3;
    oled_compose(&fb, &r);
    r.attempts_used = 4;
    oled_compose(&fb2, &r);
    TEST_ASSERT_TRUE(memcmp(fb.buf, fb2.buf, sizeof(fb.buf)) != 0);
}

TEST_CASE("the lockout countdown changes every second", "[oled]")
{
    ui_render_t r = render(UI_SCREEN_LOCKOUT);
    r.seconds_remaining = 300;
    r.progress_permille = 1000;
    oled_compose(&fb, &r);
    r.seconds_remaining = 299;
    oled_compose(&fb2, &r);
    TEST_ASSERT_TRUE(memcmp(fb.buf, fb2.buf, sizeof(fb.buf)) != 0);
}

TEST_CASE("minutes and seconds format and clamp", "[oled]")
{
    char s[6];
    oled_fmt_mmss(s, 300);
    TEST_ASSERT_EQUAL_STRING("05:00", s);
    oled_fmt_mmss(s, 59);
    TEST_ASSERT_EQUAL_STRING("00:59", s);
    oled_fmt_mmss(s, 0);
    TEST_ASSERT_EQUAL_STRING("00:00", s);
    oled_fmt_mmss(s, 100000);
    TEST_ASSERT_EQUAL_STRING("99:59", s);
}

TEST_CASE("every screen is visually distinct", "[oled]")
{
    const ui_screen_t all[] = {UI_SCREEN_IDLE,    UI_SCREEN_ENTRY,   UI_SCREEN_GRANTED,
                               UI_SCREEN_DENIED,  UI_SCREEN_NOT_YET, UI_SCREEN_EXPIRED,
                               UI_SCREEN_LOCKOUT, UI_SCREEN_NO_CLOCK};
    static oled_fb_t frames[8];
    for (int i = 0; i < 8; i++) {
        ui_render_t r = render(all[i]);
        r.progress_permille = 500;
        r.seconds_remaining = 120;
        oled_compose(&frames[i], &r);
    }
    for (int i = 0; i < 8; i++) {
        for (int j = i + 1; j < 8; j++) {
            TEST_ASSERT_TRUE(memcmp(frames[i].buf, frames[j].buf, sizeof(frames[i].buf)) != 0);
        }
    }
}

TEST_CASE("only changed pages are marked dirty", "[oled]")
{
    oled_fb_clear(&fb);
    oled_fb_clear(&fb2);
    TEST_ASSERT_EQUAL_HEX8(0x00, oled_fb_dirty_pages(&fb, &fb2));

    oled_fb_pixel(&fb2, 10, 27, true); /* page 3 */
    TEST_ASSERT_EQUAL_HEX8(0x08, oled_fb_dirty_pages(&fb, &fb2));

    /* A countdown bar tick touches one or two pages, never the frame. */
    ui_render_t r = render(UI_SCREEN_ENTRY);
    r.progress_permille = 500;
    oled_compose(&fb, &r);
    r.progress_permille = 490;
    oled_compose(&fb2, &r);
    uint8_t dirty = oled_fb_dirty_pages(&fb, &fb2);
    int pages = 0;
    for (int p = 0; p < OLED_PAGES; p++) {
        pages += (dirty >> p) & 1;
    }
    TEST_ASSERT_TRUE(pages >= 1 && pages <= 2);
}
