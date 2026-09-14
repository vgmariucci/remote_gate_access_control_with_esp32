#include "ui.h"

#include <string.h>

static uint16_t permille_remaining(uint32_t now_ms, uint32_t deadline_ms,
                                   uint32_t span_ms)
{
    if (span_ms == 0 || (int32_t)(deadline_ms - now_ms) <= 0) {
        return 0;
    }
    uint32_t remaining = deadline_ms - now_ms;
    if (remaining >= span_ms) {
        return 1000;
    }
    return (uint16_t)((remaining * 1000u) / span_ms);
}

static void go_idle(ui_ctx_t *ctx)
{
    ctx->screen = ctx->clock_trusted ? UI_SCREEN_IDLE : UI_SCREEN_NO_CLOCK;
    ctx->entry_len = 0;
    ctx->entry_deadline_ms = 0;
    ctx->screen_until_ms = 0;
}

void ui_init(ui_ctx_t *ctx, uint8_t restored_attempts,
             uint32_t restored_lockout_remaining_ms, uint32_t now_ms)
{
    if (!ctx) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->clock_trusted = false;
    ctx->attempts_used = restored_attempts > UI_MAX_ATTEMPTS
                             ? UI_MAX_ATTEMPTS
                             : restored_attempts;

    if (restored_lockout_remaining_ms > 0) {
        ctx->lockout_until_ms = now_ms + restored_lockout_remaining_ms;
        ctx->screen = UI_SCREEN_LOCKOUT;
    } else {
        ctx->lockout_until_ms = 0;
        ctx->screen = UI_SCREEN_NO_CLOCK;
    }
}

void ui_set_clock_trusted(ui_ctx_t *ctx, bool trusted)
{
    if (!ctx) {
        return;
    }
    ctx->clock_trusted = trusted;
    if (!trusted) {
        ctx->screen = UI_SCREEN_NO_CLOCK;
        ctx->entry_len = 0;
    } else if (ctx->screen == UI_SCREEN_NO_CLOCK) {
        ctx->screen = UI_SCREEN_IDLE;
    }
}

bool ui_is_locked_out(const ui_ctx_t *ctx, uint32_t now_ms)
{
    if (!ctx || ctx->lockout_until_ms == 0) {
        return false;
    }
    return (int32_t)(ctx->lockout_until_ms - now_ms) > 0;
}

ui_action_t ui_on_key(ui_ctx_t *ctx, uint32_t now_ms)
{
    if (!ctx) {
        return UI_ACTION_NONE;
    }
    /* Locked out or no trusted clock: the keypad is inert. Deliberately
     * no feedback difference, so probing tells an attacker nothing. */
    if (ui_is_locked_out(ctx, now_ms) || !ctx->clock_trusted) {
        return UI_ACTION_NONE;
    }

    /* A key during a transient screen starts a fresh entry immediately
     * rather than making the guest wait out the animation. */
    if (ctx->screen != UI_SCREEN_ENTRY) {
        ctx->entry_len = 0;
        ctx->screen = UI_SCREEN_ENTRY;
    }

    ctx->entry_len++;
    ctx->entry_deadline_ms = now_ms + UI_ENTRY_TIMEOUT_MS;

    if (ctx->entry_len >= UI_CODE_LEN) {
        return UI_ACTION_SUBMIT;
    }
    return UI_ACTION_NONE;
}

ui_action_t ui_on_long_press(ui_ctx_t *ctx, uint32_t now_ms)
{
    if (!ctx || ui_is_locked_out(ctx, now_ms)) {
        return UI_ACTION_NONE;
    }
    go_idle(ctx);
    return UI_ACTION_CLEAR_BUFFER;
}

ui_action_t ui_on_result(ui_ctx_t *ctx, ac_result_t result, uint32_t now_ms)
{
    if (!ctx) {
        return UI_ACTION_NONE;
    }
    ctx->entry_len = 0;

    switch (result) {
    case AC_GRANTED:
        ctx->attempts_used = 0;
        ctx->lockout_until_ms = 0;
        ctx->screen = UI_SCREEN_GRANTED;
        ctx->screen_until_ms = now_ms + UI_GRANTED_MS;
        break;

    /* A genuine code outside its window. The person already holds a real
     * credential, so telling them why costs nothing and saves a call.
     * It must not consume an attempt. */
    case AC_DENIED_NOT_YET:
        ctx->screen = UI_SCREEN_NOT_YET;
        ctx->screen_until_ms = now_ms + UI_INFO_MS;
        break;
    case AC_DENIED_EXPIRED:
        ctx->screen = UI_SCREEN_EXPIRED;
        ctx->screen_until_ms = now_ms + UI_INFO_MS;
        break;

    case AC_DENIED_NO_CLOCK:
        ctx->screen = UI_SCREEN_NO_CLOCK;
        ctx->screen_until_ms = 0;
        break;

    case AC_DENIED_UNKNOWN:
    case AC_DENIED_BAD_FORMAT:
    case AC_DENIED_LOCKOUT:
    default:
        if (ctx->attempts_used < UI_MAX_ATTEMPTS) {
            ctx->attempts_used++;
        }
        if (ctx->attempts_used >= UI_MAX_ATTEMPTS) {
            ctx->lockout_until_ms = now_ms + UI_LOCKOUT_MS;
            ctx->screen = UI_SCREEN_LOCKOUT;
            ctx->screen_until_ms = 0;
        } else {
            ctx->screen = UI_SCREEN_DENIED;
            ctx->screen_until_ms = now_ms + UI_DENIED_MS;
        }
        break;
    }

    return UI_ACTION_CLEAR_BUFFER;
}

ui_action_t ui_tick(ui_ctx_t *ctx, uint32_t now_ms)
{
    if (!ctx) {
        return UI_ACTION_NONE;
    }

    if (ctx->screen == UI_SCREEN_LOCKOUT) {
        if (!ui_is_locked_out(ctx, now_ms)) {
            ctx->lockout_until_ms = 0;
            ctx->attempts_used = 0; /* the lockout was the penalty */
            go_idle(ctx);
        }
        return UI_ACTION_NONE;
    }

    if (ctx->screen == UI_SCREEN_ENTRY) {
        if ((int32_t)(ctx->entry_deadline_ms - now_ms) <= 0) {
            /* Silent abandon. Costs no attempt: an incomplete entry is
             * not a wrong guess. */
            go_idle(ctx);
            return UI_ACTION_CLEAR_BUFFER;
        }
        return UI_ACTION_NONE;
    }

    if (ctx->screen_until_ms != 0 &&
        (int32_t)(ctx->screen_until_ms - now_ms) <= 0) {
        go_idle(ctx);
    }
    return UI_ACTION_NONE;
}

ui_render_t ui_render(const ui_ctx_t *ctx, uint32_t now_ms)
{
    ui_render_t r;
    memset(&r, 0, sizeof(r));
    if (!ctx) {
        return r;
    }

    r.screen = ctx->screen;
    r.attempts_used = ctx->attempts_used;

    if (ctx->screen == UI_SCREEN_ENTRY) {
        r.progress_permille = permille_remaining(now_ms, ctx->entry_deadline_ms,
                                                 UI_ENTRY_TIMEOUT_MS);
    } else if (ctx->screen == UI_SCREEN_LOCKOUT) {
        r.progress_permille = permille_remaining(now_ms, ctx->lockout_until_ms,
                                                 UI_LOCKOUT_MS);
        int32_t left = (int32_t)(ctx->lockout_until_ms - now_ms);
        r.seconds_remaining = left > 0 ? (uint32_t)((left + 999) / 1000) : 0;
    }

    return r;
}
