/*
 * sync_client - fetching the code set.
 *
 * Target-only: HTTPS, mbedtls and NVS. The decisions are elsewhere and
 * host-tested (sync_logic, sync_parse); what is here is the transport.
 *
 * ADR 0007: this poll is the correctness mechanism. Telegram only makes
 * a new code arrive sooner.
 *
 * The fetch runs in its own task, not in the main loop. A poll blocks
 * for as long as the network takes - up to HTTP_TIMEOUT, and reliably
 * that long on a marginal link. Doing that inside the main loop froze
 * the keypad for the duration and overran the task watchdog, which is
 * not what a watchdog is for: a slow network is not a hung loop.
 *
 * The lock is held only around the table and the context, never around
 * the wire. That is the whole point of the arrangement, so a future
 * change that widens it has undone the fix.
 */
#ifndef SYNC_CLIENT_H
#define SYNC_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "access_core.h"
#include "esp_err.h"
#include "sync_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *url;          /* full https URL of this gate's file */
    const char *gate_id;      /* must match the "gate" line in the body */
    const char *delivery_key; /* 64 hex characters */

    /* Everything the task needs from the rest of the firmware, passed
     * in rather than called directly, so this component keeps knowing
     * nothing about main or net_link. Same reason sync_parse takes its
     * MAC function instead of linking a crypto library. */

    void (*lock)(void);   /* guards the table and the sync context */
    void (*unlock)(void); /* must pair with lock */

    /* True while the station has an address. Asking without one buys a
     * socket and a timeout and nothing else. */
    bool (*online)(void);

    /* Milliseconds, from the same source the main loop uses. If these
     * two disagree, next_due_ms is nonsense and the poll either never
     * fires or never stops. */
    uint32_t (*now_ms)(void);
} sync_client_config_t;

/* Restores the last accepted generation from NVS. Does not fetch and
 * does not start the task. */
esp_err_t sync_client_init(const sync_client_config_t *cfg, sync_ctx_t *ctx);

/* Starts the polling task. `ctx` and `table` must outlive it, which for
 * the file-scope objects in app_main they do.
 *
 * Priority sits below the keypad and the portal's DNS so neither ever
 * waits on it, and above the main loop so an arrived code is installed
 * promptly. The task is deliberately NOT registered with the task
 * watchdog: blocking on a socket is its job. */
esp_err_t sync_client_start(sync_ctx_t *ctx, ac_ctx_t *table);

/* Forces a poll now, for the console. Blocks the calling task for the
 * length of the fetch. The caller must NOT hold the lock. */
esp_err_t sync_client_poll_now(sync_ctx_t *ctx, ac_ctx_t *table);

/* The generation currently held, for the console. */
uint64_t sync_client_generation(void);

#ifdef __cplusplus
}
#endif

#endif /* SYNC_CLIENT_H */
