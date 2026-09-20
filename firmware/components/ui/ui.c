#include "ui.h"

#include <string.h>

static uint16_t permille_remaining_ms(uint32_t now_ms, uint32_t deadline_ms, uint32_t span_ms)
{
    if (span_ms == 0 || (int32_t)(deadline_ms - now_ms) <= 0) {
        return 0;
    }
    uint32_t remaining = deadline_ms - now_ms;
    return remaining >= span_ms ? 1000 : (uint16_t)((remaining * 1000u) / span_ms);
}

static uint16_t permille_remaining_s(int64_t now, int64_t deadline, int32_t span_s)
{
    if (span_s <= 0 || deadline <= now) {
        return 0;
    }
    int64_t remaining = deadline - now;
    return remaining >= span_s ? 1000 : (uint16_t)((remaining * 1000) / span_s);
}

static void go_idle(ui_ctx_t *ctx)
{
    ctx->screen = ctx->clock_trusted ? UI_SCREEN_IDLE : UI_SCREEN_NO_CLOCK;
    ctx->entry_len = 0;
    ctx->entry_deadline_ms = 0;
    ctx->screen_until_ms = 0;
}

void ui_init(ui_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->clock_trusted = false;
    ctx->screen = UI_SCREEN_NO_CLOCK;
}

void ui_set_clock_trusted(ui_ctx_t *ctx, bool trusted)
{
    if (ctx == NULL) {
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

ui_action_t ui_on_key(ui_ctx_t *ctx, const ac_ctx_t *ac, ui_now_t now)
{
    if (ctx == NULL) {
        return UI_ACTION_NONE;
    }
    /* Inert while locked out or without trusted time. No feedback
     * difference between the two, so probing tells an attacker
     * nothing. */
    if (!ctx->clock_trusted || ac_is_locked_out(ac, now.epoch)) {
        return UI_ACTION_NONE;
    }

    /* A key during a transient screen starts a fresh entry immediately
     * rather than making the guest wait out the animation. */
    if (ctx->screen != UI_SCREEN_ENTRY) {
        ctx->entry_len = 0;
        ctx->screen = UI_SCREEN_ENTRY;
    }

    ctx->entry_len++;
    ctx->entry_deadline_ms = now.ms + UI_ENTRY_TIMEOUT_MS;

    return ctx->entry_len >= UI_CODE_LEN ? UI_ACTION_SUBMIT : UI_ACTION_NONE;
}

ui_action_t ui_on_long_press(ui_ctx_t *ctx, const ac_ctx_t *ac, ui_now_t now)
{
    if (ctx == NULL || ac_is_locked_out(ac, now.epoch)) {
        return UI_ACTION_NONE;
    }
    go_idle(ctx);
    return UI_ACTION_CLEAR_BUFFER;
}

ui_action_t ui_on_result(ui_ctx_t *ctx, const ac_ctx_t *ac, ac_result_t r, ui_now_t now)
{
    if (ctx == NULL) {
        return UI_ACTION_NONE;
    }
    ctx->entry_len = 0;

    switch (r) {
    case AC_GRANTED:
        ctx->screen = UI_SCREEN_GRANTED;
        ctx->screen_until_ms = now.ms + UI_GRANTED_MS;
        break;

    /* A genuine code outside its window. access_core charged no
     * attempt for it, so there is nothing to reflect here. */
    case AC_DENIED_NOT_YET:
        ctx->screen = UI_SCREEN_NOT_YET;
        ctx->screen_until_ms = now.ms + UI_INFO_MS;
        break;
    case AC_DENIED_EXPIRED:
        ctx->screen = UI_SCREEN_EXPIRED;
        ctx->screen_until_ms = now.ms + UI_INFO_MS;
        break;

    case AC_DENIED_NO_CLOCK:
        ctx->screen = UI_SCREEN_NO_CLOCK;
        ctx->screen_until_ms = 0;
        break;

    default: /* UNKNOWN, BAD_FORMAT, LOCKOUT */
        /* access_core already incremented and may have armed the
         * lockout. When it did, the lockout screen is derived in
         * ui_render, so idle is the correct underlying state. */
        if (ac_is_locked_out(ac, now.epoch)) {
            go_idle(ctx);
        } else {
            ctx->screen = UI_SCREEN_DENIED;
            ctx->screen_until_ms = now.ms + UI_DENIED_MS;
        }
        break;
    }

    return UI_ACTION_CLEAR_BUFFER;
}

ui_action_t ui_tick(ui_ctx_t *ctx, const ac_ctx_t *ac, ui_now_t now)
{
    if (ctx == NULL) {
        return UI_ACTION_NONE;
    }
    if (ac_is_locked_out(ac, now.epoch)) {
        ctx->entry_len = 0;
        return UI_ACTION_NONE;
    }

    if (ctx->screen == UI_SCREEN_ENTRY) {
        if ((int32_t)(ctx->entry_deadline_ms - now.ms) <= 0) {
            /* Silent abandon. Costs no attempt: an incomplete entry is
             * not a wrong guess. */
            go_idle(ctx);
            return UI_ACTION_CLEAR_BUFFER;
        }
        return UI_ACTION_NONE;
    }

    if (ctx->screen_until_ms != 0 && (int32_t)(ctx->screen_until_ms - now.ms) <= 0) {
        go_idle(ctx);
    }
    return UI_ACTION_NONE;
}

ui_render_t ui_render(const ui_ctx_t *ctx, const ac_ctx_t *ac, ui_now_t now)
{
    ui_render_t r;
    memset(&r, 0, sizeof(r));
    if (ctx == NULL || ac == NULL) {
        return r;
    }

    r.attempts_used = ac->failed_attempts;
    r.attempts_max = ac->max_failed_attempts;

    if (!ctx->clock_trusted) {
        r.screen = UI_SCREEN_NO_CLOCK;
        return r;
    }

    /* Derived, not stored: one deadline, one owner. */
    if (ac_is_locked_out(ac, now.epoch)) {
        r.screen = UI_SCREEN_LOCKOUT;
        r.progress_permille =
            permille_remaining_s(now.epoch, ac->lockout_until, ac->lockout_seconds);
        r.seconds_remaining = (uint32_t)(ac->lockout_until - now.epoch);
        return r;
    }

    r.screen = ctx->screen;
    if (ctx->screen == UI_SCREEN_ENTRY) {
        r.progress_permille =
            permille_remaining_ms(now.ms, ctx->entry_deadline_ms, UI_ENTRY_TIMEOUT_MS);
    }
    return r;
}