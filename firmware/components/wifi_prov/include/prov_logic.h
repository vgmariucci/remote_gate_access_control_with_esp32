/*
 * prov_logic - provisioning session rules and credential validation.
 *
 * Pure C99: no Wi-Fi, no HTTP, no timers. The SoftAP, the web server
 * and the captive-portal DNS are target glue on top of this.
 *
 * Two jobs:
 *
 *   1. Decide when the AP may be open. The only way in is a deliberate
 *      hold of the button inside the enclosure. There is deliberately
 *      no path from "Wi-Fi failed to connect" to "open the AP": that
 *      would let anyone with a jammer force the gate into
 *      configuration mode from the street.
 *
 *   2. Reject credentials that cannot work or should not be used,
 *      before they are written to NVS and the device reboots into a
 *      network it can never join.
 */
#ifndef PROV_LOGIC_H
#define PROV_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Deliberate hold, so closing the enclosure lid cannot arm it. */
#define PROV_HOLD_MS 5000u

/* The session is capped from the moment it opens; activity does not
 * extend it. A radio that configures the lock should be on for as
 * short a time as possible, and five minutes is long enough to type a
 * Wi-Fi password. Pressing the button again is cheap. */
#define PROV_SESSION_MS (5u * 60u * 1000u)

/* Wrong admin logins allowed before the session is torn down. Ending
 * the session (rather than blocking for a while) means an attacker in
 * radio range has to keep physical access to the button to try again. */
#define PROV_MAX_LOGIN_FAILURES 5

#define PROV_SSID_MAX 32 /* IEEE 802.11 */
#define PROV_PSK_MIN 8   /* WPA2 */
#define PROV_PSK_MAX 63  /* 64 means a raw hex PSK */

typedef enum {
    PROV_OK = 0,
    PROV_ERR_SSID_EMPTY,
    PROV_ERR_SSID_LONG,
    PROV_ERR_SSID_CONTROL,
    PROV_ERR_PSK_SHORT,
    PROV_ERR_PSK_LONG,
    PROV_ERR_PSK_CONTROL,
    PROV_ERR_AUTH_WEP,
    PROV_ERR_AUTH_UNKNOWN,
} prov_result_t;

/* Mirrors the subset of wifi_auth_mode_t we care about, so the tests
 * do not need esp_wifi.h. */
typedef enum {
    PROV_AUTH_OPEN = 0,
    PROV_AUTH_WEP,
    PROV_AUTH_WPA_PSK,
    PROV_AUTH_WPA2_PSK,
    PROV_AUTH_WPA_WPA2_PSK,
    PROV_AUTH_WPA3_PSK,
    PROV_AUTH_WPA2_WPA3_PSK,
} prov_auth_t;

prov_result_t prov_check_ssid(const char *ssid);

/* `open_network` allows an empty passphrase; otherwise WPA2 lengths
 * apply. A 64-character hex string is a raw PSK and is accepted. */
prov_result_t prov_check_psk(const char *psk, bool open_network);

/* WEP is refused outright: it is broken, and a gate that can be joined
 * by anyone who cracks it in minutes is worse than one that refuses to
 * be configured. */
prov_result_t prov_check_auth(prov_auth_t auth);

/* True when the network will work but is weaker than it should be:
 * open, or WPA-only TKIP. The portal shows this as a warning rather
 * than refusing, because it may be the only network available. */
bool prov_auth_is_weak(prov_auth_t auth);

/* One short line for the portal and the log. Never NULL. */
const char *prov_result_text(prov_result_t r);

/* ------------------------------------------------------------ session */

typedef enum {
    PROV_STATE_CLOSED = 0,
    PROV_STATE_OPEN,
} prov_state_t;

typedef enum {
    PROV_ACTION_NONE = 0,
    PROV_ACTION_OPEN_AP,  /* start SoftAP, DNS and HTTP */
    PROV_ACTION_CLOSE_AP, /* stop them, and revoke every dev code */
} prov_action_t;

typedef struct {
    prov_state_t state;
    bool button_down;
    bool button_consumed; /* one session per press */
    uint32_t button_down_ms;
    uint32_t opened_ms;
    bool authenticated;
    uint8_t login_failures;
} prov_ctx_t;

void prov_init(prov_ctx_t *ctx);

/* Feed the button state every scan. Returns OPEN_AP on the press that
 * completes the hold. */
prov_action_t prov_button(prov_ctx_t *ctx, bool down, uint32_t now_ms);

/* Drives the session timeout. Returns CLOSE_AP exactly once. */
prov_action_t prov_tick(prov_ctx_t *ctx, uint32_t now_ms);

/* Explicit end: the admin pressed "finish", or the caller is shutting
 * down. Idempotent - returns CLOSE_AP only if a session was open. */
prov_action_t prov_close(prov_ctx_t *ctx);

/* Records a login attempt. Returns true when the session is now
 * authenticated. Too many failures close the session, which the caller
 * sees on the next prov_tick. */
bool prov_login(prov_ctx_t *ctx, bool credentials_ok);

bool prov_is_open(const prov_ctx_t *ctx);
bool prov_is_authenticated(const prov_ctx_t *ctx);
uint32_t prov_seconds_left(const prov_ctx_t *ctx, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* PROV_LOGIC_H */
