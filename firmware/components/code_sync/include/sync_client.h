/*
 * sync_client - fetching the code set.
 *
 * Target-only: HTTPS, mbedtls and NVS. The decisions are elsewhere and
 * host-tested (sync_logic, sync_parse); what is here is the transport.
 *
 * ADR 0007: this poll is the correctness mechanism. Telegram, when it
 * arrives, only makes a new code arrive sooner.
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
} sync_client_config_t;

/* Restores the last accepted generation from NVS. Does not fetch. */
esp_err_t sync_client_init(const sync_client_config_t *cfg, sync_ctx_t *ctx);

/* Fetches, verifies and applies if a poll is due and the network is up.
 *
 * `table` is the live code table; the caller must hold its lock.
 * Returns true when the table changed. */
bool sync_client_tick(sync_ctx_t *ctx, ac_ctx_t *table, uint32_t now_ms, bool online);

/* Forces a poll now, for the console. */
esp_err_t sync_client_poll_now(sync_ctx_t *ctx, ac_ctx_t *table, uint32_t now_ms);

/* The generation currently held, for the console. */
uint64_t sync_client_generation(void);

#ifdef __cplusplus
}
#endif

#endif /* SYNC_CLIENT_H */
