#include "sync_parse.h"

#include <stdint.h>
#include <string.h>

static const char END_MARKER[] = "\nend\n";

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1; /* lowercase only: one encoding, one signature */
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

static bool ct_equal(const char *a, const char *b, size_t n)
{
    unsigned diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned)((unsigned char)a[i] ^ (unsigned char)b[i]);
    }
    return diff == 0;
}

static bool parse_i64(const char *s, size_t len, int64_t *out)
{
    if (len == 0 || len > 19) {
        return false;
    }
    bool negative = (s[0] == '-');
    size_t i = negative ? 1 : 0;
    if (i >= len) {
        return false;
    }
    int64_t v = 0;
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        if (v > (INT64_MAX - (s[i] - '0')) / 10) {
            return false; /* overflow */
        }
        v = v * 10 + (s[i] - '0');
    }
    *out = negative ? -v : v;
    return true;
}

static bool parse_u64(const char *s, size_t len, uint64_t *out)
{
    int64_t v = 0;
    if (!parse_i64(s, len, &v) || v < 0) {
        return false;
    }
    *out = (uint64_t)v;
    return true;
}

/* Returns the line at `*p`, advancing `*p` past it. Length excludes the
 * newline. Returns false at the end of the body. */
static bool next_line(const char **p, const char *end, const char **line, size_t *len)
{
    if (*p >= end) {
        return false;
    }
    const char *nl = memchr(*p, '\n', (size_t)(end - *p));
    *line = *p;
    *len = nl ? (size_t)(nl - *p) : (size_t)(end - *p);
    *p = nl ? nl + 1 : end;
    return true;
}

static bool starts_with(const char *line, size_t len, const char *prefix)
{
    size_t n = strlen(prefix);
    return len >= n && memcmp(line, prefix, n) == 0;
}

/* Splits "code <id> <hash> <from> <until>" on single spaces. */
static sync_parse_result_t parse_code_line(const char *line, size_t len, sync_entry_t *out)
{
    const char *field[4];
    size_t flen[4];
    size_t n = 0;

    const char *p = line + 5; /* past "code " */
    const char *end = line + len;
    const char *start = p;
    for (const char *c = p; c <= end; c++) {
        if (c == end || *c == ' ') {
            if (n >= 4) {
                return SYNC_PARSE_ENTRY;
            }
            field[n] = start;
            flen[n] = (size_t)(c - start);
            n++;
            start = c + 1;
        }
    }
    if (n != 4) {
        return SYNC_PARSE_ENTRY;
    }

    /* Exactly 8 lowercase hex: a backend id. The network can never
     * deliver something ac_id_is_dev() would call a dev code. */
    if (flen[0] != AC_ID_LEN - 1) {
        return SYNC_PARSE_ENTRY;
    }
    for (size_t i = 0; i < flen[0]; i++) {
        if (hex_nibble(field[0][i]) < 0) {
            return SYNC_PARSE_ENTRY;
        }
    }
    memcpy(out->id, field[0], flen[0]);
    out->id[flen[0]] = '\0';

    if (!hex_to_bytes(field[1], flen[1], out->hash, AC_HASH_LEN)) {
        return SYNC_PARSE_ENTRY;
    }
    if (!parse_i64(field[2], flen[2], &out->valid_from) ||
        !parse_i64(field[3], flen[3], &out->valid_until)) {
        return SYNC_PARSE_ENTRY;
    }
    if (out->valid_until < out->valid_from) {
        return SYNC_PARSE_ENTRY;
    }
    return SYNC_PARSE_OK;
}

sync_parse_result_t sync_parse(const char *body, const char *gate_id, sync_mac_fn mac,
                               void *user, sync_set_t *out)
{
    if (body == NULL || gate_id == NULL || mac == NULL || out == NULL) {
        return SYNC_PARSE_FORMAT;
    }
    memset(out, 0, sizeof(*out));

    size_t len = 0;
    while (len <= SYNC_BODY_MAX && body[len] != '\0') {
        len++;
    }
    if (len == 0 || len > SYNC_BODY_MAX) {
        return SYNC_PARSE_FORMAT;
    }

    /* The signed region runs to the end of the "end" line. A body
     * without it was cut short in transit and must not verify. */
    const char *marker = strstr(body, END_MARKER);
    if (marker == NULL) {
        return SYNC_PARSE_TRUNCATED;
    }
    size_t signed_len = (size_t)(marker - body) + sizeof(END_MARKER) - 1;

    char expected[SYNC_MAC_HEX_LEN + 1];
    if (!mac(body, signed_len, expected, user)) {
        return SYNC_PARSE_MAC;
    }

    /* The mac line is whatever follows the end marker. */
    const char *mac_line = body + signed_len;
    if (!starts_with(mac_line, (size_t)(body + len - mac_line), "mac ")) {
        return SYNC_PARSE_FORMAT;
    }
    const char *got = mac_line + 4;
    size_t got_len = (size_t)(body + len - got);
    while (got_len > 0 && (got[got_len - 1] == '\n' || got[got_len - 1] == '\r')) {
        got_len--;
    }
    if (got_len != SYNC_MAC_HEX_LEN || !ct_equal(got, expected, SYNC_MAC_HEX_LEN)) {
        return SYNC_PARSE_MAC;
    }

    /* Authenticated. Now the contents are worth reading. */
    const char *p = body;
    const char *end = body + signed_len;
    const char *line;
    size_t llen;

    static const char VERSION_LINE[] = "portao-sync v1";
    const size_t version_len = sizeof(VERSION_LINE) - 1;
    if (!next_line(&p, end, &line, &llen) || llen != version_len ||
        memcmp(line, VERSION_LINE, version_len) != 0) {
        /* The version line is fixed width; anything else is either a
         * different version or not our file at all. */
        if (llen >= 12 && memcmp(line, "portao-sync ", 12) == 0) {
            return SYNC_PARSE_VERSION;
        }
        return SYNC_PARSE_FORMAT;
    }

    if (!next_line(&p, end, &line, &llen) || !starts_with(line, llen, "gate ")) {
        return SYNC_PARSE_FORMAT;
    }
    size_t id_len = llen - 5;
    if (id_len != strlen(gate_id) || memcmp(line + 5, gate_id, id_len) != 0) {
        /* Addressed to another gate: one mis-pointed URL must not load
         * a neighbour's codes. */
        return SYNC_PARSE_GATE;
    }

    if (!next_line(&p, end, &line, &llen) || !starts_with(line, llen, "gen ") ||
        !parse_u64(line + 4, llen - 4, &out->generation)) {
        return SYNC_PARSE_FORMAT;
    }

    while (next_line(&p, end, &line, &llen)) {
        if (llen == 3 && memcmp(line, "end", 3) == 0) {
            return SYNC_PARSE_OK;
        }
        if (!starts_with(line, llen, "code ")) {
            return SYNC_PARSE_FORMAT;
        }
        if (out->count >= SYNC_MAX_ENTRIES) {
            return SYNC_PARSE_TOO_MANY;
        }
        sync_parse_result_t r = parse_code_line(line, llen, &out->entries[out->count]);
        if (r != SYNC_PARSE_OK) {
            memset(out, 0, sizeof(*out));
            return r;
        }
        out->count++;
    }
    return SYNC_PARSE_TRUNCATED;
}

const char *sync_parse_text(sync_parse_result_t r)
{
    switch (r) {
    case SYNC_PARSE_OK:
        return "ok";
    case SYNC_PARSE_FORMAT:
        return "malformed";
    case SYNC_PARSE_VERSION:
        return "unsupported version";
    case SYNC_PARSE_GATE:
        return "addressed to another gate";
    case SYNC_PARSE_MAC:
        return "signature does not verify";
    case SYNC_PARSE_TRUNCATED:
        return "body cut short";
    case SYNC_PARSE_ENTRY:
        return "bad code entry";
    case SYNC_PARSE_TOO_MANY:
        return "more codes than the device can hold";
    default:
        return "?";
    }
}
