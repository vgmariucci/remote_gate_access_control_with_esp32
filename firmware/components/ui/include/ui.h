/*
 * ui - the guest-facing state machine for the gate keypad.
 *
 * Pure C99, no display dependency. It decides WHAT should be on screen;
 * an SSD1306 renderer decides how to draw it.
 *
 * Per ADR 0004 the attempt counter and the lockout deadline live in
 * access_core and are read from here for display. The ui owns no copy:
 * two counters for one concept drift apart, and the one the guest sees
 * would not be the one the gate enforces.
 *
 * Display policy:
 *   - the entry buffer is never echoed, not even masked
 *   - a 10 s countdown bar is the only entry feedback; each keypress
 *     snaps it back to full, which confirms the press registered
 *     without revealing how many have been entered
 *   - success: a large OK for 3 s
 *   - failure: attempts used out of max, for 2 s
 *   - lockout: its own countdown, derived from ac_ctx_t
 *
 * The ui never sees the typed characters. The caller owns that buffer
 * and zeroes it; ui tracks only how many keys have been pressed.
 */
#ifndef UI_H
#define UI_H

#include "access_core.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_CODE_LEN AC_CODE_LEN
#define UI_ENTRY_TIMEOUT_MS 10000
#define UI_GRANTED_MS 3000
#define UI_DENIED_MS 2000
#define UI_INFO_MS 3000 /* "not yet" / "expired" notices */

/*
 * Two clocks, deliberately.
 *
 * `ms` is monotonic since boot and drives everything short-lived: the
 * entry timeout and the transient screens. It must not jump when NTP
 * steps the wall clock mid-entry.
 *
 * `epoch` is the trusted wall clock and is used for the lockout
 * deadline only, because that deadline is stored in the DS3232 as an
 * absolute time and has to survive a reboot.
 */
typedef struct {
    uint32_t ms;
    int64_t epoch;
} ui_now_t;

typedef enum {
    UI_SCREEN_IDLE,
    UI_SCREEN_ENTRY,
    UI_SCREEN_GRANTED,
    UI_SCREEN_DENIED,
    UI_SCREEN_NOT_YET,
    UI_SCREEN_EXPIRED,
    UI_SCREEN_LOCKOUT,
    UI_SCREEN_NO_CLOCK
} ui_screen_t;

typedef struct {
    ui_screen_t screen;
    uint16_t progress_permille; /* 0..1000, entry bar and lockout bar */
    uint8_t attempts_used;      /* read from ac_ctx_t */
    uint8_t attempts_max;       /* read from ac_ctx_t */
    uint32_t seconds_remaining; /* meaningful on LOCKOUT */
} ui_render_t;

typedef enum {
    UI_ACTION_NONE,
    UI_ACTION_SUBMIT,      /* buffer full: hash and evaluate */
    UI_ACTION_CLEAR_BUFFER /* caller must zero its character buffer */
} ui_action_t;

typedef struct {
    ui_screen_t screen;
    uint8_t entry_len;
    uint32_t entry_deadline_ms;
    uint32_t screen_until_ms;
    bool clock_trusted;
} ui_ctx_t;

/* No restored state: the lockout is recovered into ac_ctx_t by
 * ac_restore_attempts() before the ui is initialised. */
void ui_init(ui_ctx_t *ctx);

void ui_set_clock_trusted(ui_ctx_t *ctx, bool trusted);

ui_action_t ui_on_key(ui_ctx_t *ctx, const ac_ctx_t *ac, ui_now_t now);
ui_action_t ui_on_long_press(ui_ctx_t *ctx, const ac_ctx_t *ac, ui_now_t now);
ui_action_t ui_on_result(ui_ctx_t *ctx, const ac_ctx_t *ac, ac_result_t r, ui_now_t now);
ui_action_t ui_tick(ui_ctx_t *ctx, const ac_ctx_t *ac, ui_now_t now);
ui_render_t ui_render(const ui_ctx_t *ctx, const ac_ctx_t *ac, ui_now_t now);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */