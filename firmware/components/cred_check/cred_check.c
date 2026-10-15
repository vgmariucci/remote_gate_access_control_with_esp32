#include "cred_check.h"

#include <string.h>

/* Values that appear in admin_credentials.h.example, in this
 * repository, and in every tutorial. A device carrying one of these is
 * not configured; it is unlocked. */
static const char *const PLACEHOLDERS[] = {
    "admin123",
    "changeme",
    "CHANGE_ME",
    "please-change-me",
    "portao-01-placeholder",
    "0000000000000000000000000000000000000000000000000000000000000000",
    "DEV_SALT",
    "dev-salt",
    "https://example.com/gate.txt",
    /* Deliberately a well-formed token: it passes every structural
     * rule below, so only being on this list stops it. */
    "000000000:CHANGE_ME_CHANGE_ME_CHANGE_ME_CHANG",
};

static const char *const WEAK[] = {
    "password", "senha123",   "12345678", "123456789", "qwerty123",
    "admin",    "administra", "portao",   "portao123", "abc12345",
};

static bool in_list(const char *s, const char *const *list, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(s, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

static size_t len_of(const char *s)
{
    return (s == NULL) ? 0 : strlen(s);
}

/* Catches a key or salt typed as one repeated character. */
static bool all_same_char(const char *s)
{
    if (s == NULL || s[0] == '\0') {
        return true;
    }
    for (size_t i = 1; s[i] != '\0'; i++) {
        if (s[i] != s[0]) {
            return false;
        }
    }
    return true;
}

static cred_result_t common_checks(const char *s)
{
    if (len_of(s) == 0) {
        return CRED_MISSING;
    }
    if (in_list(s, PLACEHOLDERS, sizeof(PLACEHOLDERS) / sizeof(PLACEHOLDERS[0]))) {
        return CRED_PLACEHOLDER;
    }
    if (in_list(s, WEAK, sizeof(WEAK) / sizeof(WEAK[0]))) {
        return CRED_WEAK;
    }
    return CRED_OK;
}

cred_result_t cred_check_admin_password(const char *password)
{
    cred_result_t r = common_checks(password);
    if (r != CRED_OK) {
        return r;
    }
    if (len_of(password) < CRED_ADMIN_MIN_LEN) {
        return CRED_TOO_SHORT;
    }
    if (all_same_char(password)) {
        return CRED_NOT_RANDOM;
    }
    return CRED_OK;
}

cred_result_t cred_check_ap_password(const char *password)
{
    cred_result_t r = common_checks(password);
    if (r != CRED_OK) {
        return r;
    }
    size_t n = len_of(password);
    if (n < CRED_AP_MIN_LEN) {
        return CRED_TOO_SHORT;
    }
    if (n > CRED_AP_MAX_LEN) {
        return CRED_TOO_LONG; /* the radio would refuse it anyway */
    }
    if (all_same_char(password)) {
        return CRED_NOT_RANDOM;
    }
    return CRED_OK;
}

cred_result_t cred_check_delivery_key(const char *hex)
{
    cred_result_t r = common_checks(hex);
    if (r != CRED_OK) {
        return r;
    }
    if (len_of(hex) != CRED_KEY_HEX_LEN) {
        return CRED_NOT_HEX;
    }
    for (size_t i = 0; hex[i] != '\0'; i++) {
        bool ok = (hex[i] >= '0' && hex[i] <= '9') || (hex[i] >= 'a' && hex[i] <= 'f');
        if (!ok) {
            /* Uppercase is refused too: the generator writes lowercase,
             * and a key that differs only in case verifies nothing. */
            return CRED_NOT_HEX;
        }
    }
    if (all_same_char(hex)) {
        return CRED_NOT_RANDOM;
    }
    return CRED_OK;
}

cred_result_t cred_check_device_salt(const char *salt)
{
    cred_result_t r = common_checks(salt);
    if (r != CRED_OK) {
        return r;
    }
    if (len_of(salt) < CRED_SALT_MIN_LEN) {
        return CRED_TOO_SHORT;
    }
    if (all_same_char(salt)) {
        return CRED_NOT_RANDOM;
    }
    return CRED_OK;
}

cred_result_t cred_check_bot_token(const char *token)
{
    cred_result_t r = common_checks(token);
    if (r != CRED_OK) {
        return r;
    }

    /* Exactly one colon, with an id before it. A token pasted without
     * the id half is the usual way this goes wrong: the secret alone
     * looks plausible and authenticates as nobody. */
    const char *colon = strchr(token, ':');
    if (colon == NULL || colon == token || strchr(colon + 1, ':') != NULL) {
        return CRED_BAD_SHAPE;
    }

    size_t id_len = (size_t)(colon - token);
    if (id_len < CRED_TG_ID_MIN_DIGITS || id_len > CRED_TG_ID_MAX_DIGITS) {
        return CRED_BAD_SHAPE;
    }
    for (size_t i = 0; i < id_len; i++) {
        if (token[i] < '0' || token[i] > '9') {
            return CRED_BAD_SHAPE;
        }
    }

    const char *secret = colon + 1;
    size_t n = len_of(secret);
    if (n < CRED_TG_SECRET_MIN_LEN) {
        return CRED_TOO_SHORT;
    }
    if (n > CRED_TG_SECRET_MAX_LEN) {
        return CRED_TOO_LONG;
    }
    for (size_t i = 0; secret[i] != '\0'; i++) {
        char c = secret[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
        if (!ok) {
            /* Whitespace is the one that bites: a token copied out of a
             * chat window arrives with a trailing newline, goes into
             * the URL, and every request comes back 404. */
            return CRED_BAD_SHAPE;
        }
    }
    if (all_same_char(secret)) {
        return CRED_NOT_RANDOM;
    }
    return CRED_OK;
}

cred_result_t cred_check_identity(const char *gate_id, const char *url)
{
    if (len_of(gate_id) == 0 || len_of(url) == 0) {
        return CRED_MISSING;
    }
    if (in_list(gate_id, PLACEHOLDERS, sizeof(PLACEHOLDERS) / sizeof(PLACEHOLDERS[0])) ||
        in_list(url, PLACEHOLDERS, sizeof(PLACEHOLDERS) / sizeof(PLACEHOLDERS[0]))) {
        return CRED_PLACEHOLDER;
    }
    /* Plain HTTP would carry the code hashes in clear. The body is
     * signed, so this is not a forgery risk, but hashes plus a known
     * salt are worth protecting (ADR 0007). */
    if (strncmp(url, "https://", 8) != 0) {
        return CRED_WEAK;
    }
    return CRED_OK;
}

const char *cred_result_text(cred_result_t r)
{
    switch (r) {
    case CRED_OK:
        return "ok";
    case CRED_MISSING:
        return "not set";
    case CRED_TOO_SHORT:
        return "too short";
    case CRED_TOO_LONG:
        return "too long";
    case CRED_PLACEHOLDER:
        return "still the example value from the repository";
    case CRED_WEAK:
        return "on every password list ever compiled";
    case CRED_NOT_HEX:
        return "not 64 lowercase hex characters";
    case CRED_NOT_RANDOM:
        return "all one character";
    case CRED_BAD_SHAPE:
        return "not the shape this credential has to be";
    default:
        return "?";
    }
}
