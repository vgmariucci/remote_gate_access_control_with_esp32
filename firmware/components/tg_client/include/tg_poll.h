/*
 * tg_poll - when to long-poll Telegram, and how to fail at it.
 *
 * Pure C99. No network, no clock, no allocation: the caller supplies
 * now_ms and acts on the answer. The wire format lives in tg_proto; the
 * transport that uses both lives in tg_client.c and is target-only.
 *
 * ADR 0007 demoted this channel to an optimisation: the fifteen-minute
 * poll in code_sync is the correctness mechanism, and tg_client is
 * allowed to fail. Everything here follows from that. A failure backs
 * off and stays quiet rather than retrying hard, because nothing is
 * lost by waiting - the next sync repairs it. The one thing that must
 * not happen is getting stuck, so the offset advances past a message
 * even when that message was refused.
 *
 * Telegram's getUpdates takes an `offset`: the id of the first update
 * to return. Sending last_seen + 1 both asks for what is next and tells
 * the server the earlier ones were received, which is what lets them be
 * dropped server-side. Sending 0 means "whatever you still hold", which
 * is only right on a device that has consumed nothing.
 */
#ifndef TG_POLL_H
#define TG_POLL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* First retry after a failure, then doubling. */
#define TG_BACKOFF_MIN_MS 5000u

/* Ceiling on the retry interval. Deliberately below the fifteen-minute
 * sync period: once the backoff is longer than the poll it is replacing,
 * the channel has stopped being an optimisation and there is nothing to
 * gain by waiting longer. */
#define TG_BACKOFF_MAX_MS 300000u

/* What to put in getUpdates' `timeout` parameter, in seconds. The
 * request is held open by the server for this long when nothing is
 * waiting, which is how a push arrives in seconds rather than at the
 * next poll. It must stay below the HTTP client's own timeout or the
 * client gives up on a request that is working exactly as intended. */
#define TG_LONGPOLL_S 50

/* Floor between two requests, even on success.
 *
 * A long poll is supposed to be held open by the server, so the natural
 * rhythm is one request every TG_LONGPOLL_S. But a server that answers
 * 200 with an empty result immediately - misconfigured, rate-limiting,
 * or simply ignoring the timeout parameter - would otherwise make the
 * next request due at once, and the channel becomes a tight loop of TLS
 * handshakes. One second of extra latency on a path whose whole purpose
 * is to beat a fifteen-minute poll costs nothing, and it makes spinning
 * impossible by construction rather than dependent on a remote server
 * behaving. */
#define TG_MIN_GAP_MS 1000u

typedef struct {
    uint64_t offset;               /* highest update_id consumed */
    uint32_t next_due_ms;          /* earliest time the next request may start */
    uint32_t backoff_ms;           /* current retry interval; 0 when healthy */
    uint32_t consecutive_failures; /* for the log and the console */
    bool offset_dirty;             /* offset moved since it was last persisted */
    bool started;                  /* tg_poll_init has run */
    bool fatal;                    /* stopped until someone fixes something */
} tg_poll_ctx_t;

/* `stored_offset` is the value restored from NVS, or 0 on a device that
 * has never consumed an update. */
void tg_poll_init(tg_poll_ctx_t *ctx, uint64_t stored_offset, uint32_t now_ms);

/* True when a request may be issued now. Offline is never due: the
 * station owns whether there is a network, and asking without one just
 * burns a socket and a timeout. A channel that has been stopped by
 * tg_poll_fatal is never due again. */
bool tg_poll_due(const tg_poll_ctx_t *ctx, uint32_t now_ms, bool online);

/* The value to send as getUpdates' `offset`. */
uint64_t tg_poll_next_offset(const tg_poll_ctx_t *ctx);

/* A request completed, whatever it contained - an empty result is a
 * success. Clears the backoff and makes the next request due after
 * TG_MIN_GAP_MS, because a long poll is a continuous conversation rather
 * than a timer, but never a busy loop. */
void tg_poll_ok(tg_poll_ctx_t *ctx, uint32_t now_ms);

/* A request did not complete: no network, a refused connection, a
 * timeout, a body that made no sense. Doubles the backoff to its cap. */
void tg_poll_failed(tg_poll_ctx_t *ctx, uint32_t now_ms);

/* A failure that retrying cannot fix: a bot token that is wrong or has
 * been revoked, a chat that no longer exists - anything where the server
 * will keep saying no until a human changes something. 401 and 404 are
 * the ones to expect.
 *
 * Backing off to five minutes and retrying forever would mean a few
 * hundred pointless TLS handshakes a day and an invitation to be
 * rate-limited, so the channel stops instead. ADR 0007 allows that: the
 * fifteen-minute sync is the correctness mechanism and is untouched by
 * any of this. What is not allowed is stopping quietly, so the caller
 * logs this once, loudly, and `net` on the console reports it.
 *
 * There is no way back except tg_poll_init, which is deliberate: the
 * condition is a configuration error, and a reboot or a reprovision is
 * what fixes it. */
void tg_poll_fatal(tg_poll_ctx_t *ctx);

/* True once tg_poll_fatal has been called. For the log and the console. */
bool tg_poll_is_fatal(const tg_poll_ctx_t *ctx);

/* Records that `update_id` has been dealt with, and returns true when
 * that moved the offset forward.
 *
 * The caller must call this for every update it received, including the
 * ones tg_parse rejected. An update that is never consumed is returned
 * again by the next request, forever: one malformed or forged message
 * in the chat would otherwise wedge the channel permanently. Refusing
 * the contents and acknowledging receipt are different things, and only
 * the first is a security decision. */
bool tg_poll_consume(tg_poll_ctx_t *ctx, uint64_t update_id);

/* True when the offset should be written to NVS. Checked rather than
 * written every time so a quiet channel does not wear the flash. */
bool tg_poll_should_persist(const tg_poll_ctx_t *ctx);

/* Called after a successful write. */
void tg_poll_persisted(tg_poll_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TG_POLL_H */
