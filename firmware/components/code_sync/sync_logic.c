#include "sync_logic.h"

#include <string.h>

void sync_init(sync_ctx_t *ctx, uint64_t stored_generation, uint32_t now_ms)
{
    if (ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->generation = stored_generation;
    ctx->next_due_ms = now_ms; /* poll as soon as there is a network */
}

bool sync_due(const sync_ctx_t *ctx, uint32_t now_ms)
{
    if (ctx == NULL) {
        return false;
    }
    return (int32_t)(now_ms - ctx->next_due_ms) >= 0;
}

static void schedule_next(sync_ctx_t *ctx, uint32_t now_ms)
{
    ctx->next_due_ms = now_ms + SYNC_PERIOD_MS;
}

void sync_failed(sync_ctx_t *ctx, uint32_t now_ms)
{
    if (ctx == NULL) {
        return;
    }
    /* No backoff ladder here: net_retry already paces the link, and a
     * backend that is down is not helped by this device trying sooner. */
    schedule_next(ctx, now_ms);
}

void sync_unchanged(sync_ctx_t *ctx, uint32_t now_ms)
{
    if (ctx == NULL) {
        return;
    }
    ctx->last_ok_ms = now_ms;
    ctx->ever_synced = true;
    schedule_next(ctx, now_ms);
}

static bool id_is_backend(const char *id)
{
    size_t n = 0;
    while (id[n] != '\0') {
        n++;
    }
    if (n != AC_ID_LEN - 1) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) {
            return false;
        }
    }
    return true;
}

sync_result_t sync_apply(sync_ctx_t *ctx, ac_ctx_t *table, const sync_set_t *set,
                         uint32_t now_ms)
{
    if (ctx == NULL || table == NULL || set == NULL) {
        return SYNC_BAD_ENTRY;
    }
    if (set->count > SYNC_MAX_ENTRIES) {
        return SYNC_BAD_ENTRY;
    }

    if (set->generation == ctx->generation && ctx->ever_synced) {
        sync_unchanged(ctx, now_ms);
        return SYNC_UNCHANGED;
    }
    /* Strictly newer, or this is a replay. A captured set is a validly
     * signed set forever; only the generation stops it from restoring
     * a revoked code (ADR 0007). */
    if (set->generation < ctx->generation) {
        return SYNC_STALE;
    }

    /* Validate everything before touching the table: a set that is
     * half-applied is worse than one that is refused, because nobody
     * can say what the gate is holding. */
    for (size_t i = 0; i < set->count; i++) {
        if (!id_is_backend(set->entries[i].id)) {
            return SYNC_BAD_ENTRY;
        }
        if (set->entries[i].valid_until < set->entries[i].valid_from) {
            return SYNC_BAD_ENTRY;
        }
    }

    /* Replace, not merge: a code absent from the set is revoked, and
     * that is the whole reason the set is sent entire. Dev codes are
     * not the backend's to remove. */
    for (size_t i = 0; i < AC_MAX_SLOTS; i++) {
        if (table->slots[i].occupied && !table->slots[i].transient) {
            table->slots[i].occupied = false;
            memset(table->slots[i].hash, 0, AC_HASH_LEN);
            table->slots[i].id[0] = '\0';
        }
    }

    bool overflowed = false;
    for (size_t i = 0; i < set->count; i++) {
        const sync_entry_t *e = &set->entries[i];
        if (ac_upsert(table, e->id, e->hash, e->valid_from, e->valid_until) < 0) {
            /* The table is smaller than the set. Keep what fitted: a
             * partial table beats a stale one, and the log says so. */
            overflowed = true;
        }
    }

    ctx->generation = set->generation;
    ctx->last_ok_ms = now_ms;
    ctx->ever_synced = true;
    schedule_next(ctx, now_ms);
    return overflowed ? SYNC_OVERFLOWED : SYNC_APPLIED;
}

bool sync_is_stale(const sync_ctx_t *ctx, uint32_t now_ms)
{
    if (ctx == NULL || !ctx->ever_synced) {
        return true;
    }
    return (uint32_t)(now_ms - ctx->last_ok_ms) > SYNC_STALE_MS;
}
