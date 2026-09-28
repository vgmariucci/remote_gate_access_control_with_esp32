#include "access_core.h"

#include <string.h>

/* Constant-time comparison. An early-exit memcmp leaks, through timing,
 * how many leading bytes of a guessed hash were correct. The attacker
 * here is at a keypad rather than on a network, so the leak is not
 * practically exploitable, but the correct primitive costs nothing. */
static bool ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static int find_by_id(const ac_ctx_t *ctx, const char *id)
{
    for (int i = 0; i < AC_MAX_SLOTS; i++) {
        if (ctx->slots[i].occupied && strncmp(ctx->slots[i].id, id, AC_ID_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

void ac_init(ac_ctx_t *ctx, uint8_t max_failed_attempts, int32_t lockout_seconds)
{
    if (ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->max_failed_attempts = max_failed_attempts;
    ctx->lockout_seconds = lockout_seconds;
    ctx->clock_trusted = false;
}

void ac_restore_attempts(ac_ctx_t *ctx, uint8_t failed_attempts, int64_t lockout_until)
{
    if (ctx == NULL) {
        return;
    }
    ctx->failed_attempts = failed_attempts;
    ctx->lockout_until = lockout_until;
}

bool ac_is_locked_out(const ac_ctx_t *ctx, int64_t now)
{
    return ctx != NULL && ctx->lockout_until > now;
}

void ac_tick(ac_ctx_t *ctx, int64_t now)
{
    if (ctx == NULL) {
        return;
    }
    if (ctx->lockout_until != 0 && ctx->lockout_until <= now) {
        ctx->lockout_until = 0;
        ctx->failed_attempts = 0;
    }
}

bool ac_format_valid(const char *code)
{
    if (code == NULL) {
        return false;
    }

    size_t digits = 0, letters = 0, specials = 0, len = 0;

    for (const char *p = code; *p != '\0'; p++) {
        len++;
        if (len > AC_CODE_LEN) {
            return false; /* bail early rather than walking a long string */
        }
        char c = *p;
        if (c >= '0' && c <= '9') {
            digits++;
        } else if (c >= 'A' && c <= 'D') {
            letters++;
        } else if (c == '*' || c == '#') {
            specials++;
        } else {
            return false; /* not a key on a 4x4 keypad */
        }
    }

    return len == AC_CODE_LEN && digits == AC_REQUIRED_DIGITS &&
           letters == AC_REQUIRED_LETTERS && specials == AC_REQUIRED_SPECIALS;
}

static int upsert(ac_ctx_t *ctx, const char *id, const uint8_t hash[AC_HASH_LEN],
                  int64_t valid_from, int64_t valid_until, bool transient)
{
    if (ctx == NULL || id == NULL || hash == NULL || id[0] == '\0') {
        return -1;
    }
    if (valid_until <= valid_from) {
        return -1;
    }

    int idx = find_by_id(ctx, id);
    if (idx < 0) {
        for (int i = 0; i < AC_MAX_SLOTS; i++) {
            if (!ctx->slots[i].occupied) {
                idx = i;
                break;
            }
        }
    }
    if (idx < 0) {
        return -1; /* table full */
    }

    ac_slot_t *s = &ctx->slots[idx];
    s->occupied = true;
    s->transient = transient;
    strncpy(s->id, id, AC_ID_LEN - 1);
    s->id[AC_ID_LEN - 1] = '\0';
    memcpy(s->hash, hash, AC_HASH_LEN);
    s->valid_from = valid_from;
    s->valid_until = valid_until;
    return idx;
}

int ac_upsert(ac_ctx_t *ctx, const char *id, const uint8_t hash[AC_HASH_LEN],
              int64_t valid_from, int64_t valid_until)
{
    return upsert(ctx, id, hash, valid_from, valid_until, false);
}

int ac_upsert_transient(ac_ctx_t *ctx, const char *id, const uint8_t hash[AC_HASH_LEN],
                        int64_t valid_from, int64_t valid_until)
{
    /* Clamp rather than reject: the caller asked for a dev code, and a
     * shorter one is still useful. */
    if (valid_until > valid_from + AC_TRANSIENT_MAX_S) {
        valid_until = valid_from + AC_TRANSIENT_MAX_S;
    }
    return upsert(ctx, id, hash, valid_from, valid_until, true);
}

int ac_revoke_transient(ac_ctx_t *ctx)
{
    if (ctx == NULL) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < AC_MAX_SLOTS; i++) {
        if (ctx->slots[i].occupied && ctx->slots[i].transient) {
            memset(&ctx->slots[i], 0, sizeof(ac_slot_t));
            n++;
        }
    }
    return n;
}

bool ac_id_is_dev(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        return true; /* not a backend id, so not a guest's */
    }
    size_t n = 0;
    for (const char *p = id; *p != '\0'; p++, n++) {
        bool hex = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f');
        if (!hex) {
            return true;
        }
    }
    /* All hex: a backend id only if it is the right length. */
    return n != 8;
}

bool ac_revoke(ac_ctx_t *ctx, const char *id)
{
    if (ctx == NULL || id == NULL) {
        return false;
    }
    int idx = find_by_id(ctx, id);
    if (idx < 0) {
        return false;
    }
    memset(&ctx->slots[idx], 0, sizeof(ac_slot_t));
    return true;
}

int ac_purge_expired(ac_ctx_t *ctx, int64_t now)
{
    if (ctx == NULL) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < AC_MAX_SLOTS; i++) {
        if (ctx->slots[i].occupied && ctx->slots[i].valid_until < now) {
            memset(&ctx->slots[i], 0, sizeof(ac_slot_t));
            n++;
        }
    }
    return n;
}

size_t ac_count(const ac_ctx_t *ctx)
{
    if (ctx == NULL) {
        return 0;
    }
    size_t n = 0;
    for (int i = 0; i < AC_MAX_SLOTS; i++) {
        if (ctx->slots[i].occupied) {
            n++;
        }
    }
    return n;
}

static void register_failure(ac_ctx_t *ctx, int64_t now)
{
    if (ctx->failed_attempts < 255) {
        ctx->failed_attempts++;
    }
    /* The counter is NOT cleared here. It stays at the maximum so the
     * display can show "5 of 5" while the lockout runs; ac_tick clears
     * both once the deadline passes (ADR 0004). */
    if (ctx->max_failed_attempts > 0 && ctx->failed_attempts >= ctx->max_failed_attempts) {
        ctx->lockout_until = now + ctx->lockout_seconds;
    }
}

ac_result_t ac_evaluate(ac_ctx_t *ctx, const uint8_t hash[AC_HASH_LEN], int64_t now,
                        char out_id[AC_ID_LEN])
{
    if (ctx == NULL || hash == NULL) {
        return AC_DENIED_UNKNOWN;
    }

    /* Fail closed when time is not trustworthy. Checked before the
     * lockout so that the reason surfaced to the operator is the real
     * one. */
    if (!ctx->clock_trusted) {
        return AC_DENIED_NO_CLOCK;
    }

    ac_tick(ctx, now);
    if (ac_is_locked_out(ctx, now)) {
        return AC_DENIED_LOCKOUT;
    }

    /* Scan the whole table even after a match. Bailing out early would
     * make a correct-but-expired code measurably faster than an unknown
     * one. */
    int match = -1;
    for (int i = 0; i < AC_MAX_SLOTS; i++) {
        if (ctx->slots[i].occupied && ct_equal(ctx->slots[i].hash, hash, AC_HASH_LEN)) {
            match = i;
        }
    }

    if (match < 0) {
        register_failure(ctx, now);
        return AC_DENIED_UNKNOWN;
    }

    const ac_slot_t *s = &ctx->slots[match];
    /* A genuine credential outside its window. The caller already holds
     * a real code, so this leaks nothing and must not consume an
     * attempt - an early guest would otherwise lock out the gate. */
    if (now < s->valid_from) {
        return AC_DENIED_NOT_YET;
    }
    if (now > s->valid_until) {
        return AC_DENIED_EXPIRED;
    }

    ctx->failed_attempts = 0;
    ctx->lockout_until = 0;
    if (out_id != NULL) {
        memcpy(out_id, s->id, AC_ID_LEN);
    }
    return AC_GRANTED;
}
