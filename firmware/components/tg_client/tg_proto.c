#include "tg_proto.h"

#include <stdint.h>
#include <string.h>

#define FIELD_COUNT 8

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1; /* uppercase is not accepted: one encoding, one MAC */
}

static bool hex_to_bytes(const char *hex, size_t hex_len, uint8_t *out, size_t out_len)
{
    if (hex_len != out_len * 2) {
        return false;
    }
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* Constant-time, so a near-miss signature does not take longer to
 * reject than a wild one. */
static bool ct_equal(const char *a, const char *b, size_t n)
{
    unsigned diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned)((unsigned char)a[i] ^ (unsigned char)b[i]);
    }
    return diff == 0;
}

static bool parse_u64(const char *s, size_t len, uint64_t *out)
{
    if (len == 0 || len > 20) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        uint64_t next = v * 10 + (uint64_t)(s[i] - '0');
        if (next < v) {
            return false; /* overflow */
        }
        v = next;
    }
    *out = v;
    return true;
}

static bool parse_i64(const char *s, size_t len, int64_t *out)
{
    bool negative = (len > 0 && s[0] == '-');
    uint64_t magnitude = 0;
    if (!parse_u64(negative ? s + 1 : s, negative ? len - 1 : len, &magnitude)) {
        return false;
    }
    if (magnitude > (uint64_t)INT64_MAX) {
        return false;
    }
    *out = negative ? -(int64_t)magnitude : (int64_t)magnitude;
    return true;
}

tg_result_t tg_parse(const char *line, uint64_t last_counter, tg_mac_fn mac, void *user,
                     tg_msg_t *out)
{
    if (line == NULL || mac == NULL || out == NULL) {
        return TG_ERR_FORMAT;
    }
    memset(out, 0, sizeof(*out));

    /* Bounded by hand: strnlen is POSIX, and this file has to compile
     * as plain C99 wherever the tests run. */
    size_t len = 0;
    while (len <= TG_LINE_MAX && line[len] != '\0') {
        len++;
    }
    if (len == 0 || len > TG_LINE_MAX) {
        return TG_ERR_FORMAT;
    }

    /* Split on '|' without copying. */
    const char *field[FIELD_COUNT];
    size_t flen[FIELD_COUNT];
    size_t n = 0;
    const char *start = line;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || line[i] == '|') {
            if (n >= FIELD_COUNT) {
                return TG_ERR_FORMAT; /* too many fields */
            }
            field[n] = start;
            flen[n] = (size_t)(line + i - start);
            n++;
            start = line + i + 1;
        }
    }
    if (n != FIELD_COUNT) {
        return TG_ERR_FORMAT;
    }

    if (flen[0] != 2 || memcmp(field[0], "v1", 2) != 0) {
        return TG_ERR_VERSION;
    }

    /* ---- authenticate before believing anything ----
     *
     * Every check below this point acts on attacker-supplied bytes. The
     * signature covers the whole line up to the final separator, so a
     * forged message is rejected here rather than after its fields have
     * steered the parser. */
    size_t signed_len = (size_t)(field[FIELD_COUNT - 1] - line) - 1;
    char expected[TG_MAC_HEX_LEN + 1];
    if (!mac(line, signed_len, expected, user)) {
        return TG_ERR_MAC;
    }
    if (flen[FIELD_COUNT - 1] != TG_MAC_HEX_LEN ||
        !ct_equal(field[FIELD_COUNT - 1], expected, TG_MAC_HEX_LEN)) {
        return TG_ERR_MAC;
    }

    /* ---- now the contents are worth reading ---- */

    uint64_t counter = 0;
    if (!parse_u64(field[1], flen[1], &counter)) {
        return TG_ERR_FORMAT;
    }
    /* Strictly greater: re-sending an accepted message must not reapply
     * it, or revocation becomes reversible by anyone holding a copy. */
    if (counter <= last_counter) {
        return TG_ERR_REPLAY;
    }

    tg_op_t op;
    if (flen[2] == 3 && memcmp(field[2], "add", 3) == 0) {
        op = TG_OP_ADD;
    } else if (flen[2] == 3 && memcmp(field[2], "rev", 3) == 0) {
        op = TG_OP_REVOKE;
    } else {
        return TG_ERR_OP;
    }

    /* Exactly 8 lowercase hex: a backend id. The network can therefore
     * never create something that ac_id_is_dev() would call a dev code,
     * so a delivered message cannot impersonate a console session or
     * collide with one of its slots. */
    if (flen[3] != AC_ID_LEN - 1) {
        return TG_ERR_ID;
    }
    for (size_t i = 0; i < flen[3]; i++) {
        if (hex_nibble(field[3][i]) < 0) {
            return TG_ERR_ID;
        }
    }
    memcpy(out->id, field[3], flen[3]);
    out->id[flen[3]] = '\0';

    if (op == TG_OP_ADD) {
        if (!hex_to_bytes(field[4], flen[4], out->hash, AC_HASH_LEN)) {
            return TG_ERR_HASH;
        }
        if (!parse_i64(field[5], flen[5], &out->valid_from) ||
            !parse_i64(field[6], flen[6], &out->valid_until)) {
            return TG_ERR_FORMAT;
        }
        if (out->valid_until < out->valid_from) {
            return TG_ERR_WINDOW;
        }
    }
    /* A revoke carries an id and nothing else worth reading; the
     * remaining fields exist only so every message has one shape and
     * one signed layout. */

    out->counter = counter;
    out->op = op;
    return TG_OK;
}

const char *tg_result_text(tg_result_t r)
{
    switch (r) {
    case TG_OK:
        return "ok";
    case TG_ERR_FORMAT:
        return "malformed";
    case TG_ERR_VERSION:
        return "unsupported version";
    case TG_ERR_MAC:
        return "signature does not verify";
    case TG_ERR_REPLAY:
        return "replayed or out of order";
    case TG_ERR_ID:
        return "not a backend id";
    case TG_ERR_HASH:
        return "bad hash";
    case TG_ERR_WINDOW:
        return "inverted validity window";
    case TG_ERR_OP:
        return "unknown operation";
    default:
        return "?";
    }
}
