#include "tg_poll.h"

/* Deadlines are compared as a signed difference rather than with `<`.
 * now_ms is a 32-bit millisecond counter, so it wraps roughly every 49
 * days, and a gate is expected to run for months. A plain comparison
 * stops the channel dead for the 49 days after the first wrap, which is
 * the kind of fault that only shows up in the field. */
static bool reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

void tg_poll_init(tg_poll_ctx_t *ctx, uint64_t stored_offset, uint32_t now_ms)
{
    if (ctx == NULL) {
        return;
    }
    ctx->offset = stored_offset;
    ctx->next_due_ms = now_ms;
    ctx->backoff_ms = 0;
    ctx->consecutive_failures = 0;
    ctx->offset_dirty = false;
    ctx->started = true;

    /* Init is the only way out of the fatal state: a wrong token is
     * fixed by reprovisioning, not by waiting. */
    ctx->fatal = false;
}

bool tg_poll_due(const tg_poll_ctx_t *ctx, uint32_t now_ms, bool online)
{
    if (ctx == NULL || !ctx->started || ctx->fatal || !online) {
        return false;
    }
    return reached(now_ms, ctx->next_due_ms);
}

uint64_t tg_poll_next_offset(const tg_poll_ctx_t *ctx)
{
    /* 0 is not an offset of 0, it is "send what you have". Asking for
     * offset 1 on a fresh device would skip whatever is already queued. */
    if (ctx == NULL || ctx->offset == 0) {
        return 0;
    }
    return ctx->offset + 1;
}

void tg_poll_ok(tg_poll_ctx_t *ctx, uint32_t now_ms)
{
    if (ctx == NULL) {
        return;
    }
    ctx->backoff_ms = 0;
    ctx->consecutive_failures = 0;
    ctx->next_due_ms = now_ms + TG_MIN_GAP_MS;
}

void tg_poll_fatal(tg_poll_ctx_t *ctx)
{
    if (ctx != NULL) {
        ctx->fatal = true;
    }
}

bool tg_poll_is_fatal(const tg_poll_ctx_t *ctx)
{
    return ctx != NULL && ctx->fatal;
}

void tg_poll_failed(tg_poll_ctx_t *ctx, uint32_t now_ms)
{
    if (ctx == NULL) {
        return;
    }
    if (ctx->consecutive_failures < UINT32_MAX) {
        ctx->consecutive_failures++;
    }

    uint32_t next;
    if (ctx->backoff_ms == 0) {
        next = TG_BACKOFF_MIN_MS;
    } else if (ctx->backoff_ms > TG_BACKOFF_MAX_MS / 2) {
        /* Clamped before doubling rather than after, so the multiply
         * cannot overflow into a small interval and turn the backoff
         * into a tight retry loop. */
        next = TG_BACKOFF_MAX_MS;
    } else {
        next = ctx->backoff_ms * 2;
    }
    ctx->backoff_ms = next;
    ctx->next_due_ms = now_ms + next;
}

bool tg_poll_consume(tg_poll_ctx_t *ctx, uint64_t update_id)
{
    if (ctx == NULL || update_id <= ctx->offset) {
        return false;
    }
    ctx->offset = update_id;
    ctx->offset_dirty = true;
    return true;
}

bool tg_poll_should_persist(const tg_poll_ctx_t *ctx)
{
    return ctx != NULL && ctx->offset_dirty;
}

void tg_poll_persisted(tg_poll_ctx_t *ctx)
{
    if (ctx != NULL) {
        ctx->offset_dirty = false;
    }
}
