/*
 * sync_logic - when to poll, and what to do with the answer.
 *
 * Pure C99. The transport (HTTPS, JSON, mbedtls) lives elsewhere; what
 * is here is the part worth testing: the generation ordering that stops
 * a replayed set from restoring a revoked code, and the rule that a
 * newer set replaces the table rather than merging into it (ADR 0007).
 */
#ifndef SYNC_LOGIC_H
#define SYNC_LOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "access_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYNC_PERIOD_MS (15u * 60u * 1000u)
#define SYNC_STALE_MS (60u * 60u * 1000u) /* amber past this */
#define SYNC_MAX_ENTRIES 8                /* the backend may send more than we hold */

typedef struct {
    char id[AC_ID_LEN];
    uint8_t hash[AC_HASH_LEN];
    int64_t valid_from;
    int64_t valid_until;
} sync_entry_t;

typedef struct {
    uint64_t generation;
    size_t count;
    sync_entry_t entries[SYNC_MAX_ENTRIES];
} sync_set_t;

typedef enum {
    SYNC_APPLIED = 0,
    SYNC_UNCHANGED,  /* same generation: nothing to do */
    SYNC_STALE,      /* older generation: replayed or out of order */
    SYNC_BAD_ENTRY,  /* an id or window that cannot be trusted */
    SYNC_OVERFLOWED, /* applied, but the set did not fit */
} sync_result_t;

typedef struct {
    uint64_t generation; /* highest accepted; persisted in NVS */
    uint32_t last_ok_ms; /* when a sync last succeeded */
    bool ever_synced;
    uint32_t next_due_ms;
} sync_ctx_t;

void sync_init(sync_ctx_t *ctx, uint64_t stored_generation, uint32_t now_ms);

/* True when a poll is due. Connectivity is the caller's business. */
bool sync_due(const sync_ctx_t *ctx, uint32_t now_ms);

/* A poll finished without an answer: try again a period later rather
 * than hammering a backend that is down. */
void sync_failed(sync_ctx_t *ctx, uint32_t now_ms);

/* Installs `set` into `table`, replacing every guest code.
 *
 * Transient (dev) codes are left alone: they belong to a console or
 * portal session and have their own revocation triggers.
 *
 * An entry is refused if the id is not a backend id, which keeps the
 * network from creating something that looks like a dev code. */
sync_result_t sync_apply(sync_ctx_t *ctx, ac_ctx_t *table, const sync_set_t *set,
                         uint32_t now_ms);

/* The backend said nothing changed. Counts as a successful sync. */
void sync_unchanged(sync_ctx_t *ctx, uint32_t now_ms);

/* True when the last successful sync is old enough to be worth
 * flagging, or none has ever happened. */
bool sync_is_stale(const sync_ctx_t *ctx, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* SYNC_LOGIC_H */
