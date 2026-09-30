#include "prov_logic.h"

#include <string.h>

static bool is_printable_ascii(const char *s, size_t *out_len)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++, n++) {
        /* Control characters and DEL. Non-ASCII bytes are allowed in an
         * SSID: the standard treats it as opaque bytes, and plenty of
         * routers use accented names. */
        if (*p < 0x20 || *p == 0x7F) {
            return false;
        }
    }
    if (out_len != NULL) {
        *out_len = n;
    }
    return true;
}

prov_result_t prov_check_ssid(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return PROV_ERR_SSID_EMPTY;
    }
    size_t n = 0;
    if (!is_printable_ascii(ssid, &n)) {
        return PROV_ERR_SSID_CONTROL;
    }
    if (n > PROV_SSID_MAX) {
        return PROV_ERR_SSID_LONG;
    }
    return PROV_OK;
}

static bool is_hex64(const char *s, size_t n)
{
    if (n != 64) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

prov_result_t prov_check_psk(const char *psk, bool open_network)
{
    if (psk == NULL) {
        psk = "";
    }
    size_t n = 0;
    if (!is_printable_ascii(psk, &n)) {
        return PROV_ERR_PSK_CONTROL;
    }
    if (open_network) {
        /* An open network takes no passphrase; anything typed is a
         * mistake worth reporting rather than silently dropping. */
        return n == 0 ? PROV_OK : PROV_ERR_PSK_LONG;
    }
    if (is_hex64(psk, n)) {
        return PROV_OK; /* raw PSK */
    }
    if (n < PROV_PSK_MIN) {
        return PROV_ERR_PSK_SHORT;
    }
    if (n > PROV_PSK_MAX) {
        return PROV_ERR_PSK_LONG;
    }
    return PROV_OK;
}

prov_result_t prov_check_auth(prov_auth_t auth)
{
    switch (auth) {
    case PROV_AUTH_WEP:
        return PROV_ERR_AUTH_WEP;
    case PROV_AUTH_OPEN:
    case PROV_AUTH_WPA_PSK:
    case PROV_AUTH_WPA2_PSK:
    case PROV_AUTH_WPA_WPA2_PSK:
    case PROV_AUTH_WPA3_PSK:
    case PROV_AUTH_WPA2_WPA3_PSK:
        return PROV_OK;
    default:
        return PROV_ERR_AUTH_UNKNOWN;
    }
}

bool prov_auth_is_weak(prov_auth_t auth)
{
    return auth == PROV_AUTH_OPEN || auth == PROV_AUTH_WPA_PSK;
}

const char *prov_result_text(prov_result_t r)
{
    switch (r) {
    case PROV_OK:
        return "ok";
    case PROV_ERR_SSID_EMPTY:
        return "pick a network, or type a name for a hidden one";
    case PROV_ERR_SSID_LONG:
        return "network name is longer than 32 characters";
    case PROV_ERR_SSID_CONTROL:
        return "network name contains control characters";
    case PROV_ERR_PSK_SHORT:
        return "password is shorter than 8 characters";
    case PROV_ERR_PSK_LONG:
        return "password is longer than 63 characters";
    case PROV_ERR_PSK_CONTROL:
        return "password contains control characters";
    case PROV_ERR_AUTH_WEP:
        return "WEP is not supported: it can be broken in minutes";
    case PROV_ERR_AUTH_UNKNOWN:
        return "unsupported security type";
    default:
        return "invalid";
    }
}

/* ------------------------------------------------------------ session */

void prov_init(prov_ctx_t *ctx)
{
    if (ctx != NULL) {
        memset(ctx, 0, sizeof(*ctx));
    }
}

static prov_action_t close_session(prov_ctx_t *ctx)
{
    if (ctx->state != PROV_STATE_OPEN) {
        return PROV_ACTION_NONE;
    }
    ctx->state = PROV_STATE_CLOSED;
    ctx->authenticated = false;
    ctx->login_failures = 0;
    ctx->opened_ms = 0;
    return PROV_ACTION_CLOSE_AP;
}

prov_action_t prov_button(prov_ctx_t *ctx, bool down, uint32_t now_ms)
{
    if (ctx == NULL) {
        return PROV_ACTION_NONE;
    }

    if (!down) {
        /* Release re-arms the button, so a session that ends while the
         * button is still held does not immediately reopen. */
        ctx->button_down = false;
        ctx->button_consumed = false;
        return PROV_ACTION_NONE;
    }

    if (!ctx->button_down) {
        ctx->button_down = true;
        ctx->button_down_ms = now_ms;
        return PROV_ACTION_NONE;
    }
    if (ctx->button_consumed || ctx->state == PROV_STATE_OPEN) {
        return PROV_ACTION_NONE;
    }
    if ((uint32_t)(now_ms - ctx->button_down_ms) < PROV_HOLD_MS) {
        return PROV_ACTION_NONE;
    }

    ctx->button_consumed = true;
    ctx->state = PROV_STATE_OPEN;
    ctx->opened_ms = now_ms;
    ctx->authenticated = false;
    ctx->login_failures = 0;
    return PROV_ACTION_OPEN_AP;
}

prov_action_t prov_tick(prov_ctx_t *ctx, uint32_t now_ms)
{
    if (ctx == NULL || ctx->state != PROV_STATE_OPEN) {
        return PROV_ACTION_NONE;
    }
    if ((uint32_t)(now_ms - ctx->opened_ms) >= PROV_SESSION_MS) {
        return close_session(ctx);
    }
    return PROV_ACTION_NONE;
}

prov_action_t prov_close(prov_ctx_t *ctx)
{
    return ctx == NULL ? PROV_ACTION_NONE : close_session(ctx);
}

bool prov_login(prov_ctx_t *ctx, bool credentials_ok)
{
    if (ctx == NULL || ctx->state != PROV_STATE_OPEN) {
        return false;
    }
    if (credentials_ok) {
        ctx->authenticated = true;
        ctx->login_failures = 0;
        return true;
    }
    if (ctx->login_failures < 255) {
        ctx->login_failures++;
    }
    if (ctx->login_failures >= PROV_MAX_LOGIN_FAILURES) {
        close_session(ctx);
    }
    return false;
}

bool prov_is_open(const prov_ctx_t *ctx)
{
    return ctx != NULL && ctx->state == PROV_STATE_OPEN;
}

bool prov_is_authenticated(const prov_ctx_t *ctx)
{
    return ctx != NULL && ctx->state == PROV_STATE_OPEN && ctx->authenticated;
}

uint32_t prov_seconds_left(const prov_ctx_t *ctx, uint32_t now_ms)
{
    if (ctx == NULL || ctx->state != PROV_STATE_OPEN) {
        return 0;
    }
    uint32_t elapsed = (uint32_t)(now_ms - ctx->opened_ms);
    if (elapsed >= PROV_SESSION_MS) {
        return 0;
    }
    return (PROV_SESSION_MS - elapsed) / 1000u;
}
