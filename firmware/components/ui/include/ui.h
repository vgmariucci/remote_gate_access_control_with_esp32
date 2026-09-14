/*
 * ui - the guest-facing state machine for the gate keypad.
 *
 * Pure C99, no display dependency. It decides WHAT should be on screen;
 * an SSD1306 renderer decides how to draw it. Same reason as access_core:
 * every transition and timeout below is testable on a CI runner.
 *
 * Display policy, per spec:
 *   - the entry buffer is never echoed, not even masked. Length must not
 *     leak to a bystander.
 *   - a 10 s countdown bar is the only entry feedback. Each keypress
 *     snaps it back to full, which confirms the press registered without
 *     revealing how many have been entered.
 *   - success: a large OK for 3 s.
 *   - failure: attempts used out of 5, for 2 s.
 *   - fifth failure: keypad dead for 5 minutes, with its own countdown.
 *
 * The ui never sees the typed characters. The caller owns that buffer and
 * zeroes it; ui tracks only how many keys have been pressed.
 */
#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stdint.h>
#include "access_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_CODE_LEN        9      /* fixed: 6 digits + 2 letters + 1 special */
#define UI_ENTRY_TIMEOUT_MS 10000
#define UI_GRANTED_MS      3000
#define UI_DENIED_MS       2000
#define UI_INFO_MS         3000   /* "not yet" / "expired" notices */
#define UI_MAX_ATTEMPTS    5
#define UI_LOCKOUT_MS      300000 /* 5 minutes */

typedef enum {
    UI_SCREEN_IDLE,      /* invite to type */
    UI_SCREEN_ENTRY,     /* countdown bar, nothing else */
    UI_SCREEN_GRANTED,   /* big OK */
    UI_SCREEN_DENIED,    /* "tentativa N de 5" */
    UI_SCREEN_NOT_YET,   /* genuine code, before its window */
    UI_SCREEN_EXPIRED,   /* genuine code, after its window */
    UI_SCREEN_LOCKOUT,   /* blocked, with countdown */
    UI_SCREEN_NO_CLOCK   /* fail closed: time not trusted */
} ui_screen_t;

typedef struct {
    ui_screen_t screen;
    /* 0..1000 for the entry bar and the lockout bar. Integer permille so
     * the renderer needs no float and the tests compare exactly. */
    uint16_t    progress_permille;
    uint8_t     attempts_used;     /* meaningful on DENIED and LOCKOUT */
    uint32_t    seconds_remaining; /* meaningful on LOCKOUT */
} ui_render_t;

typedef enum {
    UI_ACTION_NONE,
    UI_ACTION_SUBMIT,      /* buffer reached UI_CODE_LEN: hash and evaluate */
    UI_ACTION_CLEAR_BUFFER /* caller must zero its character buffer */
} ui_action_t;

typedef struct {
    ui_screen_t screen;
    uint8_t     entry_len;
    uint32_t    entry_deadline_ms;
    uint32_t    screen_until_ms;   /* when a transient screen expires */
    uint8_t     attempts_used;
    uint32_t    lockout_until_ms;
    bool        clock_trusted;
} ui_ctx_t;

/*
 * `attempts_used` is restored from the DS3232 battery-backed SRAM, not
 * zeroed, so that cutting power does not reset the lockout.
 */
void ui_init(ui_ctx_t *ctx, uint8_t restored_attempts,
             uint32_t restored_lockout_remaining_ms, uint32_t now_ms);

void ui_set_clock_trusted(ui_ctx_t *ctx, bool trusted);

/* A debounced key arrived. The caller appends the character to its own
 * buffer only when this returns UI_ACTION_NONE or UI_ACTION_SUBMIT. */
ui_action_t ui_on_key(ui_ctx_t *ctx, uint32_t now_ms);

/* Long press: abandon the current entry. */
ui_action_t ui_on_long_press(ui_ctx_t *ctx, uint32_t now_ms);

/* Feed the verdict from ac_evaluate() after a SUBMIT. */
ui_action_t ui_on_result(ui_ctx_t *ctx, ac_result_t result, uint32_t now_ms);

/* Drive timeouts. Call from the main loop, ~50 ms is plenty. */
ui_action_t ui_tick(ui_ctx_t *ctx, uint32_t now_ms);

/* What the renderer should draw right now. */
ui_render_t ui_render(const ui_ctx_t *ctx, uint32_t now_ms);

/* True while the keypad should be ignored entirely. */
bool ui_is_locked_out(const ui_ctx_t *ctx, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
