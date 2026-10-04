/*
 * net_retry - when to try the house Wi-Fi again.
 *
 * Pure C99. A gate that cannot reach its router must keep trying: the
 * router will come back, and nobody is going to walk out and press a
 * button when it does. But retrying every second forever is how a
 * device ends up blamed for a congested network, and it burns power
 * for nothing.
 *
 * So: exponential backoff from one second, doubling, capped. It never
 * gives up, because giving up means a gate that stays offline until
 * someone notices.
 *
 * Note what this does NOT gate: opening the lock. Validation is
 * offline (ADR 0001), so a gate with no network still works. What it
 * delays is the clock sync and the arrival of new codes.
 */
#ifndef NET_RETRY_H
#define NET_RETRY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NET_RETRY_FIRST_MS 1000u
#define NET_RETRY_MAX_MS 60000u

/* Attempts before the log drops to one line per retry rather than one
 * per attempt. The gate keeps trying either way. */
#define NET_RETRY_QUIET_AFTER 5

typedef struct {
    bool connected;
    uint8_t failures;     /* saturates; only the backoff cares */
    uint32_t delay_ms;    /* current backoff */
    uint32_t next_try_ms; /* when the next attempt is due */
} net_retry_t;

/* Ready to try immediately. */
void net_retry_init(net_retry_t *r, uint32_t now_ms);

/* True when an attempt is due. False while backing off, and false
 * while connected. */
bool net_retry_due(const net_retry_t *r, uint32_t now_ms);

/* Record the outcome of an attempt. */
void net_retry_failed(net_retry_t *r, uint32_t now_ms);
void net_retry_connected(net_retry_t *r);

/* The link dropped: try again immediately rather than waiting out the
 * backoff from whenever it last failed. A router that reboots should
 * not cost a minute of downtime. */
void net_retry_disconnected(net_retry_t *r, uint32_t now_ms);

/* True when this failure deserves a log line. */
bool net_retry_should_log(const net_retry_t *r);

uint32_t net_retry_delay_ms(const net_retry_t *r);

#ifdef __cplusplus
}
#endif

#endif /* NET_RETRY_H */
