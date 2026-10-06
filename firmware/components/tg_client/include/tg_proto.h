/*
 * tg_proto - the wire format for codes delivered over Telegram.
 *
 * Pure C99. The bot is an untrusted courier: anyone who finds the chat
 * can post into it, and Telegram itself sees every byte. So the device
 * trusts nothing but the MAC.
 *
 * One line, all ASCII, pipe-separated:
 *
 *   v1|<counter>|<op>|<id>|<hash_hex>|<from>|<until>|<mac_hex>
 *
 *   counter   monotonic, decimal. Replay protection.
 *   op        "add" or "rev"
 *   id        exactly 8 lowercase hex characters (a backend id)
 *   hash_hex  64 hex characters: sha256(code || device_salt)
 *   from,until unix seconds, UTC, decimal
 *   mac_hex   64 hex characters: hmac-sha256 over everything before
 *             the final '|', with the device's delivery key
 *
 * The MAC covers the counter, so a captured message cannot be replayed
 * with a different one. Without the counter, revocation would be
 * reversible by anyone who kept a copy of the original add.
 *
 * The plaintext code never appears on the wire: the backend sends the
 * hash, and the guest gets the code by another channel.
 */
#ifndef TG_PROTO_H
#define TG_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "access_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TG_LINE_MAX 256
#define TG_MAC_HEX_LEN 64

typedef enum {
    TG_OK = 0,
    TG_ERR_FORMAT,  /* not the right shape at all */
    TG_ERR_VERSION, /* a version this build does not speak */
    TG_ERR_MAC,     /* signature does not verify: forged or corrupt */
    TG_ERR_REPLAY,  /* counter at or below the last accepted */
    TG_ERR_ID,      /* not an 8-hex backend id */
    TG_ERR_HASH,    /* not 64 hex characters */
    TG_ERR_WINDOW,  /* inverted or implausible validity window */
    TG_ERR_OP,      /* neither add nor rev */
} tg_result_t;

typedef enum {
    TG_OP_ADD = 0,
    TG_OP_REVOKE,
} tg_op_t;

typedef struct {
    uint64_t counter;
    tg_op_t op;
    char id[AC_ID_LEN];
    uint8_t hash[AC_HASH_LEN];
    int64_t valid_from;
    int64_t valid_until;
} tg_msg_t;

/* Computes the MAC over `len` bytes of `signed_region` and writes 64
 * lowercase hex characters plus NUL. Returns false if it could not.
 *
 * Injected rather than called directly so this file stays free of any
 * crypto library, and so the tests can drive both a matching and a
 * mismatching signature without owning a key. */
typedef bool (*tg_mac_fn)(const char *signed_region, size_t len,
                          char out_hex[TG_MAC_HEX_LEN + 1], void *user);

/* Parses and authenticates one line.
 *
 * `last_counter` is the highest counter accepted so far; anything at or
 * below it is a replay. Pass 0 on a device that has accepted nothing.
 *
 * The MAC is checked before any field is believed, so a forged message
 * cannot steer the parser with its contents. */
tg_result_t tg_parse(const char *line, uint64_t last_counter, tg_mac_fn mac, void *user,
                     tg_msg_t *out);

/* Human-readable, for the log. Never includes the line itself. */
const char *tg_result_text(tg_result_t r);

#ifdef __cplusplus
}
#endif

#endif /* TG_PROTO_H */
