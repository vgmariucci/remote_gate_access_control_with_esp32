/*
 * tg_client - the Telegram courier's transport.
 *
 * Target-only: HTTPS, cJSON, mbedtls and NVS. The decisions are
 * elsewhere and host-tested - tg_proto for the wire format, tg_poll for
 * the cadence and the failure policy.
 *
 * ADR 0007 demoted this channel to an optimisation: the fifteen-minute
 * poll in code_sync is the correctness mechanism, and this is allowed
 * to fail. So nothing here is ever fatal. A bad token, a dead network,
 * a bot that no longer exists: the channel logs it, switches itself
 * off, and the gate carries on opening for valid codes.
 *
 * Like sync_client, the fetch runs in its own task and the lock is
 * taken only around the moment the table changes - never around the
 * wire. A long poll holds a request open for TG_LONGPOLL_S, which is
 * fifty seconds the main loop must not spend waiting.
 *
 * Two numbers are persisted and they are NOT the same number:
 *
 *   offset   Telegram's update_id. A receipt: it says "I received
 *            this", including for messages that were refused.
 *   counter  tg_proto's counter, inside the signed region. The replay
 *            boundary. It moves only when a message authenticates.
 *
 * Collapsing them into one would look like a tidy-up and would quietly
 * kill the replay defence: acknowledging a forgery would advance the
 * number that decides what a forger can replay.
 */
#ifndef TG_CLIENT_H
#define TG_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "access_core.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* From @BotFather: <digits>:<secret>. Checked with
     * cred_check_bot_token; a failure disables the channel rather than
     * stopping the gate. */
    const char *bot_token;

    /* The same 64 hex characters sync_client uses. One delivery key,
     * two transports: a code pushed over Telegram and the same code
     * fetched from the backend carry the same signature. */
    const char *delivery_key;

    void (*lock)(void);   /* gate_app_lock: guards the code table */
    void (*unlock)(void); /* must pair with lock */

    bool (*online)(void);     /* net_link_sta_connected */
    uint32_t (*now_ms)(void); /* the same base the main loop uses */
} tg_client_config_t;

/* Validates the token and restores both numbers from NVS. Does not
 * poll and does not start the task.
 *
 * Returns ESP_OK when the channel is usable. Any other result means it
 * is off for this boot, which is a logged event, not a failure the
 * caller needs to handle. */
esp_err_t tg_client_init(const tg_client_config_t *cfg);

/* Starts the long-poll task. `table` must outlive it.
 *
 * Priority sits below sync_client's, which is below the portal's DNS
 * and the keypad: when both channels want to install a code at once,
 * the one that is the correctness mechanism goes first. Not registered
 * with the task watchdog - blocking on a socket is the job. */
esp_err_t tg_client_start(ac_ctx_t *table);

/* For `tg` on the dev console. */
bool tg_client_enabled(void);
uint64_t tg_client_offset(void);
uint64_t tg_client_counter(void);
const char *tg_client_state_text(void);

#ifdef __cplusplus
}
#endif

#endif /* TG_CLIENT_H */
