/*
 * access_core - gate access decision logic.
 *
 * Deliberately free of any ESP-IDF dependency: pure C99, no allocation,
 * no I/O, no time source of its own. Everything here runs identically on
 * the ESP32 and on a GitHub runner, which is what makes milestone 1
 * testable without hardware.
 *
 * Pipeline the caller is expected to follow:
 *
 *   1. keypad assembles a typed string
 *   2. ac_format_valid()  - cheap reject before spending cycles on sha256
 *   3. caller computes sha256(typed || device_salt)
 *   4. ac_evaluate()      - the decision
 *   5. caller drives the relay and logs the attempt
 *
 * The plaintext never reaches this module, and never reaches flash.
 */
#ifndef ACCESS_CORE_H
#define ACCESS_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AC_HASH_LEN 32  /* sha256 */
#define AC_MAX_SLOTS 16 /* concurrent guests per gate; fixed => no heap */
#define AC_ID_LEN 9     /* 8 hex chars + NUL, matches access_code.id prefix */

/* Password policy, format_version 1 (mirrors the DB column). */
#define AC_REQUIRED_DIGITS 6
#define AC_MIN_LETTERS 2 /* from A-D */
#define AC_MIN_SPECIALS 1 /* from * # */
#define AC_MAX_CODE_LEN 16

typedef enum {
    AC_GRANTED = 0,
    AC_DENIED_UNKNOWN,   /* hash not in the table */
    AC_DENIED_NOT_YET,   /* known, before valid_from */
    AC_DENIED_EXPIRED,   /* known, after valid_until */
    AC_DENIED_LOCKOUT,   /* too many recent failures */
    AC_DENIED_NO_CLOCK,  /* no trusted time: fail closed */
    AC_DENIED_BAD_FORMAT /* returned by the caller, never by ac_evaluate */
} ac_result_t;

typedef struct {
    bool occupied;
    char id[AC_ID_LEN];
    uint8_t hash[AC_HASH_LEN];
    int64_t valid_from;  /* unix seconds, UTC */
    int64_t valid_until; /* unix seconds, UTC */
} ac_slot_t;

typedef struct {
    ac_slot_t slots[AC_MAX_SLOTS];

    /* Trusted-time flag. Set by the time module once NTP or the DS3231
     * has produced a plausible reading. While false, every code is
     * refused: a controller that cannot tell the time cannot enforce an
     * expiry, and silently accepting codes would be worse than a locked
     * gate. */
    bool clock_trusted;

    uint8_t failed_attempts;
    int64_t lockout_until; /* unix seconds; 0 = not locked out */

    /* Policy, injected so tests can use small numbers. */
    uint8_t max_failed_attempts;
    int32_t lockout_seconds;
} ac_ctx_t;

/* Zeroes the table and applies the lockout policy. */
void ac_init(ac_ctx_t *ctx, uint8_t max_failed_attempts, int32_t lockout_seconds);

/* True when `code` satisfies format_version 1. NULL-safe. */
bool ac_format_valid(const char *code);

/* Insert or replace by id. Returns the slot index, or -1 when the table
 * is full or the arguments are invalid. Replacing by id is what makes a
 * re-push after sync_failed idempotent. */
int ac_upsert(ac_ctx_t *ctx, const char *id, const uint8_t hash[AC_HASH_LEN],
              int64_t valid_from, int64_t valid_until);

/* Removes the slot with this id. Returns true when something was removed. */
bool ac_revoke(ac_ctx_t *ctx, const char *id);

/* Frees slots whose window closed before `now`. Returns how many. */
int ac_purge_expired(ac_ctx_t *ctx, int64_t now);

/* Number of occupied slots. */
size_t ac_count(const ac_ctx_t *ctx);

/* The decision. On AC_GRANTED, `out_id` (may be NULL) receives the id of
 * the matching slot so the caller can attribute the attempt.
 *
 * Side effects: bumps failed_attempts and arms the lockout on denial,
 * clears both on success. */
ac_result_t ac_evaluate(ac_ctx_t *ctx, const uint8_t hash[AC_HASH_LEN], int64_t now,
                        char out_id[AC_ID_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* ACCESS_CORE_H */
