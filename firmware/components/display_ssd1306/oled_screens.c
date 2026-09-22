#include "oled_screens.h"

#include <stdio.h>

/* All guest-facing text in one place. */
static const char *const STR_DENIED = "SENHA INVALIDA";
static const char *const STR_TRY = "TENTATIVA";
static const char *const STR_NOT_YET_1 = "AINDA NAO";
static const char *const STR_NOT_YET_2 = "VALIDA";
static const char *const STR_EXPIRED_1 = "SENHA";
static const char *const STR_EXPIRED_2 = "EXPIRADA";
static const char *const STR_LOCKED = "BLOQUEADO";
static const char *const STR_NO_CLOCK_1 = "SEM RELOGIO";
static const char *const STR_NO_CLOCK_2 = "AGUARDE";
static const char *const STR_OK = "OK";

void oled_fmt_mmss(char out[6], uint32_t seconds)
{
    if (seconds > 99u * 60u + 59u) {
        seconds = 99u * 60u + 59u;
    }
    snprintf(out, 6, "%02u:%02u", (unsigned)(seconds / 60), (unsigned)(seconds % 60));
}

static void draw_bar(oled_fb_t *fb, int y, uint16_t permille)
{
    if (permille > 1000) {
        permille = 1000;
    }
    oled_fb_rect(fb, OLED_BAR_X, y, OLED_BAR_W, OLED_BAR_H);
    int fill = (int)((OLED_BAR_INNER_W * (uint32_t)permille) / 1000u);
    oled_fb_fill_rect(fb, OLED_BAR_X + 2, y + 2, fill, OLED_BAR_H - 4, true);
}

void oled_compose(oled_fb_t *fb, const ui_render_t *r)
{
    oled_fb_clear(fb);
    if (fb == NULL || r == NULL) {
        return;
    }

    char line[24];

    switch (r->screen) {
    case UI_SCREEN_IDLE:
        /* Deliberately empty. The panel should also be powered down
         * (ui_render_t.panel_on == false); drawing nothing here is the
         * second line of defence if it is not. */
        break;

    case UI_SCREEN_ENTRY:
        /* The bar and nothing else. No dots, no count, no cursor: the
         * entry length must not be recoverable from the screen. */
        draw_bar(fb, (OLED_H - OLED_BAR_H) / 2, r->progress_permille);
        break;

    case UI_SCREEN_GRANTED:
        oled_fb_text_centered(fb, (OLED_H - 7 * 5) / 2, STR_OK, 5);
        break;

    case UI_SCREEN_DENIED:
        oled_fb_text_centered(fb, 10, STR_DENIED, 1);
        snprintf(line, sizeof(line), "%s %u/%u", STR_TRY, (unsigned)r->attempts_used,
                 (unsigned)r->attempts_max);
        oled_fb_text_centered(fb, 34, line, 1);
        break;

    case UI_SCREEN_NOT_YET:
        oled_fb_text_centered(fb, 14, STR_NOT_YET_1, 2);
        oled_fb_text_centered(fb, 36, STR_NOT_YET_2, 2);
        break;

    case UI_SCREEN_EXPIRED:
        oled_fb_text_centered(fb, 14, STR_EXPIRED_1, 2);
        oled_fb_text_centered(fb, 36, STR_EXPIRED_2, 2);
        break;

    case UI_SCREEN_LOCKOUT:
        oled_fb_text_centered(fb, 2, STR_LOCKED, 2);
        oled_fmt_mmss(line, r->seconds_remaining);
        oled_fb_text_centered(fb, 22, line, 2);
        draw_bar(fb, OLED_H - OLED_BAR_H - 2, r->progress_permille);
        break;

    case UI_SCREEN_NO_CLOCK:
    default:
        oled_fb_text_centered(fb, 14, STR_NO_CLOCK_1, 1);
        oled_fb_text_centered(fb, 36, STR_NO_CLOCK_2, 1);
        break;
    }
}
